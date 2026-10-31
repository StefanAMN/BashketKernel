/**
 * @file virtio_pci.c
 * @brief virtio over PCI, legacy and modern (see virtio_pci.h)
 */

#include "virtio_pci.h"
#include "virtqueue.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "kprintf.h"
#include "kstring.h"
#include "interrupts.h"
#include "percpu.h"
#include "smp.h"

// --- PCI configuration space (PCI Local Bus Specification 3.0, section 6) -----

#define PCI_STATUS            0x06
#define PCI_STATUS_CAP_LIST   (1u << 4)  // The device has a capability list
#define PCI_CAPABILITIES      0x34       // Offset of its first entry
#define PCI_CAP_ID_VENDOR     0x09       // An entry whose meaning the vendor defines
#define PCI_CAP_ID_MSIX       0x11

// The MSI-X capability (PCI 3.0, section 6.8.2), as offsets within it
#define MSIX_CONTROL          2          // 16 bits: Message Control
#define MSIX_TABLE            4          // 32 bits: table offset (bits 31:3) and BIR (bits 2:0)
#define MSIX_CONTROL_SIZE     0x07FF     // Table size minus one
#define MSIX_CONTROL_MASK_ALL (1u << 14) // Function Mask: no entry may send while set
#define MSIX_CONTROL_ENABLE   (1u << 15) // MSI-X Enable: the INTx line is no longer used
// A table entry (6.8.2.6 to 6.8.2.9): 16 bytes
#define MSIX_ENTRY_SIZE       16
#define MSIX_ENTRY_ADDR_LOW   0
#define MSIX_ENTRY_ADDR_HIGH  4
#define MSIX_ENTRY_DATA       8
#define MSIX_ENTRY_CONTROL    12         // Bit 0: this entry is masked

// A virtio capability (virtio 1.2, section 4.1.4), as offsets within the entry
#define CAP_NEXT        1
#define CAP_LENGTH      2
#define CAP_CFG_TYPE    3
#define CAP_BAR         4
#define CAP_OFFSET      8
#define CAP_STRUCT_LEN  12
#define CAP_NOTIFY_MULT 16   // Notification capabilities only

#define CFG_TYPE_COMMON 1
#define CFG_TYPE_NOTIFY 2
#define CFG_TYPE_ISR    3
#define CFG_TYPE_DEVICE 4

// The common configuration structure (section 4.1.4.3), as offsets
#define COMMON_DEVICE_FEATURE_SELECT 0   // 32 bits: which half of the feature bits the next field shows
#define COMMON_CONFIG_MSIX_VECTOR    16  // 16 bits: MSI-X entry for configuration changes
#define COMMON_DEVICE_FEATURE        4   // 32 bits, read
#define COMMON_DRIVER_FEATURE_SELECT 8
#define COMMON_DRIVER_FEATURE        12  // 32 bits, write
#define COMMON_NUM_QUEUES            18  // 16 bits
#define COMMON_DEVICE_STATUS         20  // 8 bits
#define COMMON_CONFIG_GENERATION     21  // 8 bits: changes whenever the device configuration does
#define COMMON_QUEUE_SELECT          22  // 16 bits: which queue the fields below are about
#define COMMON_QUEUE_SIZE            24  // 16 bits
#define COMMON_QUEUE_MSIX_VECTOR     26  // 16 bits: MSI-X entry for the selected queue
#define COMMON_QUEUE_ENABLE          28  // 16 bits
#define COMMON_QUEUE_NOTIFY_OFF      30  // 16 bits, read
#define COMMON_QUEUE_DESC            32  // 64 bits: physical address of the descriptor table
#define COMMON_QUEUE_DRIVER          40  // 64 bits: of the available ring
#define COMMON_QUEUE_DEVICE          48  // 64 bits: of the used ring
#define COMMON_LENGTH                56

#define MAX_STRUCTURE_LENGTH (1u << 20)  // Nothing here is anywhere near a megabyte

// Registers in memory are read and written with accesses of exactly their
// own width (section 4.1.3.1), 64-bit ones as two halves
static inline uint8_t mmio8(volatile uint8_t *base, uint32_t offset) { return base[offset]; }
// Read a 16-bit register at `offset` from `base`
static inline uint16_t mmio16(volatile uint8_t *base, uint32_t offset) { return *(volatile uint16_t *)(base + offset); }
// Read a 32-bit register at `offset` from `base`
static inline uint32_t mmio32(volatile uint8_t *base, uint32_t offset) { return *(volatile uint32_t *)(base + offset); }
// Write an 8-bit register at `offset` from `base`
static inline void mmio_write8(volatile uint8_t *base, uint32_t offset, uint8_t v) { base[offset] = v; }
// Write a 16-bit register at `offset` from `base`
static inline void mmio_write16(volatile uint8_t *base, uint32_t offset, uint16_t v) { *(volatile uint16_t *)(base + offset) = v; }
// Write a 32-bit register at `offset` from `base`
static inline void mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t v) { *(volatile uint32_t *)(base + offset) = v; }
// (A 64-bit register is written as two 32-bit halves, low first)
static inline void mmio_write64(volatile uint8_t *base, uint32_t offset, uint64_t v) {
    mmio_write32(base, offset, (uint32_t)v);
    mmio_write32(base, offset + 4, (uint32_t)(v >> 32));
}

bool virtio_pci_find(uint16_t type, unsigned n, pci_device_t *out) {
    // Devices that also offer the legacy interface keep their old ids;
    // modern-only ones are numbered from 0x1040 (section 4.1.2.1)
    const uint16_t ids[2] = { (uint16_t)(0x1000 + type - 1), (uint16_t)(0x1040 + type) };
    for (int i = 0; i < 2; i++) {
        for (unsigned k = 0; pci_find_nth_device(VIRTIO_VENDOR, ids[i], k, out); k++) {
            if (n-- == 0) return true;
        }
    }
    return false;
}

bool virtio_pci_parse_caps(uint8_t (*read8)(void *ctx, uint8_t offset), uint32_t (*read32)(void *ctx, uint8_t offset),
                           void *ctx, virtio_caps_t *out) {
    memset(out, 0, sizeof(*out));
    if (!(read8(ctx, PCI_STATUS) & PCI_STATUS_CAP_LIST)) return false;

    // Each entry names the next. Configuration space has 256 bytes and an
    // entry takes at least 4, so more than 64 steps means the list loops.
    uint8_t at = read8(ctx, PCI_CAPABILITIES) & 0xFC;
    for (int steps = 0; at >= 0x40 && steps < 64; steps++) {
        uint8_t id = read8(ctx, at);
        uint8_t next = read8(ctx, (uint8_t)(at + CAP_NEXT)) & 0xFC;
        uint8_t entry_length = read8(ctx, (uint8_t)(at + CAP_LENGTH));

        if (id == PCI_CAP_ID_VENDOR && entry_length >= 16 && at <= 256 - 16) {
            virtio_cap_t cap = {
                .bar = read8(ctx, (uint8_t)(at + CAP_BAR)),
                .offset = read32(ctx, (uint8_t)(at + CAP_OFFSET)),
                .length = read32(ctx, (uint8_t)(at + CAP_STRUCT_LEN)),
                .found = true,
            };
            uint8_t type = read8(ctx, (uint8_t)(at + CAP_CFG_TYPE));

            // A structure must lie in a BAR that exists, and be of a size
            // that can be mapped without wrapping around
            bool sane = cap.bar <= 5 && cap.length > 0 && cap.length <= MAX_STRUCTURE_LENGTH &&
                        cap.offset <= 0xFFFFFFFFu - cap.length;

            // The first usable one of each kind is the one to use (4.1.4.1)
            if (sane && type == CFG_TYPE_COMMON && !out->common.found && cap.length >= COMMON_LENGTH) {
                out->common = cap;
            } else if (sane && type == CFG_TYPE_NOTIFY && !out->notify.found && cap.length >= 2 &&
                       entry_length >= 20 && at <= 256 - 20) {
                out->notify = cap;
                out->notify_multiplier = read32(ctx, (uint8_t)(at + CAP_NOTIFY_MULT));
            } else if (sane && type == CFG_TYPE_ISR && !out->isr.found) {
                out->isr = cap;
            } else if (sane && type == CFG_TYPE_DEVICE && !out->device.found) {
                out->device = cap;
            }
        }
        at = next;
    }
    return out->common.found && out->notify.found && out->isr.found && out->device.found;
}

bool virtio_pci_parse_msix(uint8_t (*read8)(void *ctx, uint8_t offset), uint32_t (*read32)(void *ctx, uint8_t offset),
                           void *ctx, virtio_msix_cap_t *out) {
    memset(out, 0, sizeof(*out));
    if (!(read8(ctx, PCI_STATUS) & PCI_STATUS_CAP_LIST)) return false;

    // The same walk as for the virtio capabilities, with the same limits
    uint8_t at = read8(ctx, PCI_CAPABILITIES) & 0xFC;
    for (int steps = 0; at >= 0x40 && steps < 64; steps++) {
        if (read8(ctx, at) == PCI_CAP_ID_MSIX && at <= 256 - 12) {
            uint16_t control = (uint16_t)(read32(ctx, at) >> 16);
            uint32_t table = read32(ctx, (uint8_t)(at + MSIX_TABLE));
            out->offset = at;
            out->entries = (uint16_t)((control & MSIX_CONTROL_SIZE) + 1);
            out->bar = (uint8_t)(table & 7);
            out->table_offset = table & ~7u;
            // BIR values 6 and 7 are reserved; the table must fit its BAR's
            // 32-bit offset range without wrapping
            out->found = out->bar <= 5 &&
                         out->table_offset <= 0xFFFFFFFFu - (uint32_t)out->entries * MSIX_ENTRY_SIZE;
            return out->found;
        }
        at = read8(ctx, (uint8_t)(at + CAP_NEXT)) & 0xFC;
    }
    return false;
}

// The real configuration space, for virtio_pci_parse_caps (`ctx` is the device)
static uint8_t config_read8(void *ctx, uint8_t offset) { return pci_read8((const pci_device_t *)ctx, offset); }
// 32 bits of the real configuration space, for virtio_pci_parse_caps (`ctx` is the device)
static uint32_t config_read32(void *ctx, uint8_t offset) { return pci_read32((const pci_device_t *)ctx, offset); }

// Map `length` bytes at `offset` in memory BAR number `index` (0..5) and
// return where they now are, or NULL
static volatile uint8_t *map_bar(const pci_device_t *pci, uint8_t index, uint32_t offset, uint32_t length) {
    if (index > 5) return NULL;
    uint32_t bar = pci_read32(pci, (uint8_t)(PCI_BAR0 + index * 4));
    if (bar & 1) return NULL; // An I/O port region: allowed by the specification, not offered by anything we run on

    // Bits 2:1 of a memory BAR give its width: 2 means 64 bits, with the
    // upper half in the next BAR (PCI 3.0, section 6.2.5.1)
    uint64_t base = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2) {
        if (index >= 5) return NULL;
        base |= (uint64_t)pci_read32(pci, (uint8_t)(PCI_BAR0 + (index + 1) * 4)) << 32;
    }
    if (base == 0) return NULL; // The firmware did not give the region an address

    return (volatile uint8_t *)vmm_map_device(base + offset, length);
}

// Map one of the structures (it lies in one of the device's memory regions)
// and return where it now is, or NULL
static volatile uint8_t *map_structure(const pci_device_t *pci, const virtio_cap_t *cap) {
    return map_bar(pci, cap->bar, cap->offset, cap->length);
}

// Write the device status register (VIRTIO_STATUS_* bits; 0 resets it)
static void set_status(virtio_device_t *dev, uint8_t status) {
    if (dev->modern) mmio_write8(dev->common, COMMON_DEVICE_STATUS, status);
    else outb(dev->io_base + VIRTIO_REG_DEVICE_STATUS, status);
}

// Read the device status register
static uint8_t get_status(virtio_device_t *dev) {
    return dev->modern ? mmio8(dev->common, COMMON_DEVICE_STATUS) : inb(dev->io_base + VIRTIO_REG_DEVICE_STATUS);
}

bool virtio_open(virtio_device_t *dev, const pci_device_t *pci) {
    memset(dev, 0, sizeof(*dev));
    dev->pci = *pci;
    for (int i = 0; i < VIRTIO_MAX_QUEUES; i++) dev->queue_vector[i] = VIRTIO_MSI_NO_VECTOR; // (0 is a real entry)
    dev->irq = pci_read8(pci, PCI_INTERRUPT_LINE);

    virtio_caps_t caps;
    if (virtio_pci_parse_caps(config_read8, config_read32, (void *)pci, &caps)) {
        pci_enable_memory_and_bus_master(pci);
        dev->common = map_structure(pci, &caps.common);
        dev->notify = map_structure(pci, &caps.notify);
        dev->isr = map_structure(pci, &caps.isr);
        dev->device_config = map_structure(pci, &caps.device);
        dev->notify_length = caps.notify.length;
        dev->device_config_length = caps.device.length;
        dev->notify_multiplier = caps.notify_multiplier;
        dev->modern = dev->common && dev->notify && dev->isr && dev->device_config;
    }
    if (!dev->modern) {
        uint32_t bar0 = pci_read32(pci, PCI_BAR0);
        if (!(bar0 & 1)) return false; // Neither interface
        dev->io_base = (uint16_t)(bar0 & ~3u);
        pci_enable_io_and_bus_master(pci);
    }

    // Reset, then announce ourselves step by step. A modern device may
    // take a moment to reset: it says so by not reading back 0 yet.
    set_status(dev, 0);
    for (int i = 0; dev->modern && i < 100000 && get_status(dev) != 0; i++) asm volatile("pause");
    if (get_status(dev) != 0) return false;
    set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);
    set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    return true;
}

bool virtio_negotiate(virtio_device_t *dev, uint64_t wanted, uint64_t *accepted) {
    uint64_t offered, agreed;
    if (dev->modern) {
        mmio_write32(dev->common, COMMON_DEVICE_FEATURE_SELECT, 0);
        offered = mmio32(dev->common, COMMON_DEVICE_FEATURE);
        mmio_write32(dev->common, COMMON_DEVICE_FEATURE_SELECT, 1);
        offered |= (uint64_t)mmio32(dev->common, COMMON_DEVICE_FEATURE) << 32;

        // A device reached through this interface that does not offer
        // VERSION_1 is not one we know how to talk to
        if (!(offered & VIRTIO_F_VERSION_1)) {
            virtio_fail(dev);
            return false;
        }
        agreed = (offered & wanted) | VIRTIO_F_VERSION_1;
        mmio_write32(dev->common, COMMON_DRIVER_FEATURE_SELECT, 0);
        mmio_write32(dev->common, COMMON_DRIVER_FEATURE, (uint32_t)agreed);
        mmio_write32(dev->common, COMMON_DRIVER_FEATURE_SELECT, 1);
        mmio_write32(dev->common, COMMON_DRIVER_FEATURE, (uint32_t)(agreed >> 32));

        // The device has the last word: it keeps FEATURES_OK set only if
        // it can work with that combination (section 3.1.1, steps 5 and 6)
        set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
        if (!(get_status(dev) & VIRTIO_STATUS_FEATURES_OK)) {
            virtio_fail(dev);
            return false;
        }
    } else {
        offered = inl(dev->io_base + VIRTIO_REG_DEVICE_FEATURES);
        agreed = offered & wanted & 0xFFFFFFFFULL; // The legacy interface has 32 feature bits
        outl(dev->io_base + VIRTIO_REG_DRIVER_FEATURES, (uint32_t)agreed);
    }
    if (accepted) *accepted = agreed;
    return true;
}

void virtio_driver_ok(virtio_device_t *dev) {
    set_status(dev, (uint8_t)(get_status(dev) | VIRTIO_STATUS_DRIVER_OK));
}

void virtio_fail(virtio_device_t *dev) {
    set_status(dev, VIRTIO_STATUS_FAILED);
}

uint8_t virtio_isr(virtio_device_t *dev) {
    return dev->modern ? mmio8(dev->isr, 0) : inb(dev->io_base + VIRTIO_REG_ISR_STATUS);
}

uint8_t virtio_config8(virtio_device_t *dev, uint32_t offset) {
    if (!dev->modern) return offset < 0x100 ? inb((uint16_t)(dev->io_base + VIRTIO_REG_DEVICE_CONFIG + offset)) : 0;
    return offset < dev->device_config_length ? mmio8(dev->device_config, offset) : 0;
}

uint64_t virtio_config64(virtio_device_t *dev, uint32_t offset) {
    if (!dev->modern) {
        if (offset >= 0x100) return 0;
        uint16_t port = (uint16_t)(dev->io_base + VIRTIO_REG_DEVICE_CONFIG + offset);
        return (uint64_t)inl(port) | ((uint64_t)inl((uint16_t)(port + 4)) << 32);
    }
    if (offset > dev->device_config_length || dev->device_config_length - offset < 8) return 0;

    // Two reads can straddle a change. The device counts its changes: read
    // again until the count was the same before and after (section 4.1.4.3.1).
    uint64_t value = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t before = mmio8(dev->common, COMMON_CONFIG_GENERATION);
        value = (uint64_t)mmio32(dev->device_config, offset) | ((uint64_t)mmio32(dev->device_config, offset + 4) << 32);
        if (mmio8(dev->common, COMMON_CONFIG_GENERATION) == before) break;
    }
    return value;
}

const char *virtio_interface_name(const virtio_device_t *dev) {
    return dev->modern ? "modern" : "legacy";
}

// --- MSI-X ------------------------------------------------------------------------

bool virtio_msix_start(virtio_device_t *dev) {
    if (!dev->modern) return false;
    virtio_msix_cap_t cap;
    if (!virtio_pci_parse_msix(config_read8, config_read32, (void *)&dev->pci, &cap)) return false;
    volatile uint8_t *table = map_bar(&dev->pci, cap.bar, cap.table_offset, (uint32_t)cap.entries * MSIX_ENTRY_SIZE);
    if (!table) return false;

    // On, with the whole function masked while the entries are filled in
    // (6.8.2.3): nothing can be sent half-written
    uint16_t control = (uint16_t)(pci_read32(&dev->pci, cap.offset) >> 16);
    pci_write16(&dev->pci, (uint8_t)(cap.offset + MSIX_CONTROL),
                (uint16_t)(control | MSIX_CONTROL_ENABLE | MSIX_CONTROL_MASK_ALL));
    for (uint16_t i = 0; i < cap.entries; i++) mmio_write32(table, (uint32_t)i * MSIX_ENTRY_SIZE + MSIX_ENTRY_CONTROL, 1);

    // Configuration changes (a disk resized) are not acted on: no message for them
    mmio_write16(dev->common, COMMON_CONFIG_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

    dev->msix = true;
    dev->msix_cap = cap.offset;
    dev->msix_entries = cap.entries;
    dev->msix_table = table;
    return true;
}

bool virtio_msix_assign(virtio_device_t *dev, uint16_t queue, uint16_t entry, uint8_t vector, uint32_t cpu) {
    if (!dev->msix || queue >= VIRTIO_MAX_QUEUES || entry >= dev->msix_entries) return false;
    cpu_t *target = cpu_get(cpu);
    uint64_t address;
    uint32_t data;
    if (!target || !msi_message(target->lapic_id, vector, &address, &data)) return false;

    uint32_t at = (uint32_t)entry * MSIX_ENTRY_SIZE;
    mmio_write32(dev->msix_table, at + MSIX_ENTRY_ADDR_LOW, (uint32_t)address);
    mmio_write32(dev->msix_table, at + MSIX_ENTRY_ADDR_HIGH, (uint32_t)(address >> 32));
    mmio_write32(dev->msix_table, at + MSIX_ENTRY_DATA, data);
    dev->queue_vector[queue] = entry;
    dev->vector = vector;
    dev->vector_cpu = cpu;
    return true;
}

// Give up on MSI-X: the device goes back to its line
static void msix_stop(virtio_device_t *dev) {
    uint16_t control = (uint16_t)(pci_read32(&dev->pci, dev->msix_cap) >> 16);
    pci_write16(&dev->pci, (uint8_t)(dev->msix_cap + MSIX_CONTROL),
                (uint16_t)(control & ~(MSIX_CONTROL_ENABLE | MSIX_CONTROL_MASK_ALL)));
    dev->msix = false;
    dev->vector = 0;
}

bool virtio_msix_finish(virtio_device_t *dev) {
    if (!dev->msix) return false;
    bool any = false;
    for (uint16_t q = 0; q < VIRTIO_MAX_QUEUES; q++) {
        if (dev->queue_vector[q] == VIRTIO_MSI_NO_VECTOR) continue;
        mmio_write16(dev->common, COMMON_QUEUE_SELECT, q);
        if (mmio16(dev->common, COMMON_QUEUE_MSIX_VECTOR) != dev->queue_vector[q]) {
            kprintf("virtio: the device refused MSI-X entry %u for queue %u; using its line\n", dev->queue_vector[q], q);
            msix_stop(dev);
            return false;
        }
        any = true;
    }
    if (!any) {
        msix_stop(dev);
        return false;
    }
    for (uint16_t q = 0; q < VIRTIO_MAX_QUEUES; q++) {
        if (dev->queue_vector[q] == VIRTIO_MSI_NO_VECTOR) continue;
        mmio_write32(dev->msix_table, (uint32_t)dev->queue_vector[q] * MSIX_ENTRY_SIZE + MSIX_ENTRY_CONTROL, 0);
    }
    uint16_t control = (uint16_t)(pci_read32(&dev->pci, dev->msix_cap) >> 16);
    pci_write16(&dev->pci, (uint8_t)(dev->msix_cap + MSIX_CONTROL), (uint16_t)(control & ~MSIX_CONTROL_MASK_ALL));
    return true;
}

void virtio_describe_interrupts(const virtio_device_t *dev, char *buf, size_t size) {
    if (dev->msix) ksnprintf(buf, size, "MSI-X vector 0x%x on CPU %u", dev->vector, dev->vector_cpu);
    else ksnprintf(buf, size, "line IRQ %u", dev->irq);
}

// --- Queues: the part of virtqueue_setup that differs ---------------------------

uint16_t virtio_queue_size(virtio_device_t *dev, uint16_t index) {
    if (dev->modern) {
        if (index >= mmio16(dev->common, COMMON_NUM_QUEUES)) return 0;
        mmio_write16(dev->common, COMMON_QUEUE_SELECT, index);
        return mmio16(dev->common, COMMON_QUEUE_SIZE);
    }
    outw(dev->io_base + VIRTIO_REG_QUEUE_SELECT, index);
    return inw(dev->io_base + VIRTIO_REG_QUEUE_SIZE);
}

bool virtio_queue_start(virtio_device_t *dev, uint16_t index, uint64_t desc_phys, uint64_t avail_phys, uint64_t used_phys,
                        volatile uint16_t **notify_address) {
    *notify_address = NULL;
    if (!dev->modern) {
        // One register: the page number of the descriptor table, with the
        // rings at fixed places after it
        if ((desc_phys >> 12) > 0xFFFFFFFFULL) return false;
        outw(dev->io_base + VIRTIO_REG_QUEUE_SELECT, index);
        outl(dev->io_base + VIRTIO_REG_QUEUE_ADDRESS, (uint32_t)(desc_phys >> 12));
        return true;
    }

    mmio_write16(dev->common, COMMON_QUEUE_SELECT, index);

    // Where this queue is notified: its own offset, in units the
    // capability gave. It has to be inside the structure we mapped.
    uint64_t notify_offset = (uint64_t)mmio16(dev->common, COMMON_QUEUE_NOTIFY_OFF) * dev->notify_multiplier;
    if (notify_offset + 2 > dev->notify_length) return false;
    *notify_address = (volatile uint16_t *)(dev->notify + notify_offset);

    // Which MSI-X entry its interrupts go through, before it is enabled
    // (4.1.5.1.2: the device reads back VIRTIO_MSI_NO_VECTOR if it cannot
    // take it; virtio_msix_finish checks)
    mmio_write16(dev->common, COMMON_QUEUE_MSIX_VECTOR,
                 index < VIRTIO_MAX_QUEUES ? dev->queue_vector[index] : VIRTIO_MSI_NO_VECTOR);

    mmio_write64(dev->common, COMMON_QUEUE_DESC, desc_phys);
    mmio_write64(dev->common, COMMON_QUEUE_DRIVER, avail_phys);
    mmio_write64(dev->common, COMMON_QUEUE_DEVICE, used_phys);
    mmio_write16(dev->common, COMMON_QUEUE_ENABLE, 1);
    return true;
}
