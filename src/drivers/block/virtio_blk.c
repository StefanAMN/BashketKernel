/**
 * @file virtio_blk.c
 * @brief virtio block device (disk) driver, legacy PCI interface
 *
 * How a request travels (virtio 1.0 specification, sections 2.4 and 5.2,
 * "legacy interface"):
 *
 *   The driver and the device share a "virtqueue": three arrays in ordinary
 *   memory whose physical address the driver gives to the device.
 *
 *     descriptor table   each entry describes one buffer: address, length,
 *                        whether the device may write to it, and optionally
 *                        the index of the next buffer of the same request
 *     available ring     written by the driver: "request starting at
 *                        descriptor N is ready for you"
 *     used ring          written by the device: "request N is done"
 *
 *   A disk request is a chain of three buffers:
 *     1. a 16-byte header: read or write, and the sector number
 *     2. the data: 512 bytes per sector
 *     3. one status byte, which the device sets to 0 on success
 *
 *   The driver fills the descriptors, adds the chain to the available ring,
 *   and writes to the "notify" port. The device does the I/O, adds an entry
 *   to the used ring and raises an interrupt.
 *
 * The device reads and writes these buffers by physical address (DMA), so
 * they live in frames taken straight from the frame allocator, and data is
 * copied between them and the caller's buffer.
 */

#include "virtio_blk.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "kprintf.h"
#include "kstring.h"
#include "sched.h"
#include "spinlock.h"
#include "pit.h"
#include "ioapic.h"
#include "interrupts.h"

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_BLK_LEGACY_ID   0x1001

// Legacy virtio registers, as offsets from the I/O base in BAR0
#define REG_DEVICE_FEATURES 0x00 // 32 bits, read
#define REG_DRIVER_FEATURES 0x04 // 32 bits, write
#define REG_QUEUE_ADDRESS   0x08 // 32 bits: physical page number of the virtqueue
#define REG_QUEUE_SIZE      0x0C // 16 bits, read
#define REG_QUEUE_SELECT    0x0E // 16 bits
#define REG_QUEUE_NOTIFY    0x10 // 16 bits
#define REG_DEVICE_STATUS   0x12 // 8 bits
#define REG_ISR_STATUS      0x13 // 8 bits; reading it acknowledges the interrupt
#define REG_CAPACITY        0x14 // 64 bits: disk size in sectors (device-specific area)

#define STATUS_ACKNOWLEDGE 0x01 // "I have noticed you"
#define STATUS_DRIVER      0x02 // "I know how to drive you"
#define STATUS_DRIVER_OK   0x04 // "I am ready"
#define STATUS_FAILED      0x80

#define DESC_F_NEXT  1 // The chain continues at `next`
#define DESC_F_WRITE 2 // The device writes to this buffer (otherwise it only reads it)

#define BLK_T_IN  0 // Read from the disk
#define BLK_T_OUT 1 // Write to the disk

#define QUEUE_ALIGN 4096
#define SECTORS_PER_REQUEST (PAGE_SIZE / VIRTIO_BLK_SECTOR_SIZE) // The data buffer is one page

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;      // Where the driver will put the next entry (only ever increases)
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;      // Where the device will put the next entry
    struct vring_used_elem ring[];
} __attribute__((packed));

struct blk_request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static bool present = false;
static uint16_t io_base;
static uint64_t capacity_sectors;
static uint16_t queue_size;

static struct vring_desc *desc;
static struct vring_avail *avail;
static volatile struct vring_used *used;
static uint16_t last_used_idx;       // How much of the used ring has been seen

// DMA buffers: one page for the header and status byte, one for data
static struct blk_request_header *req_header;
static volatile uint8_t *req_status;
static uint8_t *req_data;
static uint64_t req_header_phys, req_status_phys, req_data_phys;

static bool busy = false;            // A request is in flight
static uint64_t completed_requests = 0;
static volatile uint64_t interrupt_count = 0;

static void virtio_blk_irq(struct registers *regs) {
    (void)regs;
    // Reading the ISR register is what makes the device lower its interrupt
    // line. Bit 0 set means the used ring changed.
    uint8_t isr = inb(io_base + REG_ISR_STATUS);
    if (isr & 1) {
        interrupt_count++;
        sched_wakeup(&used);
    }
}

int virtio_blk_init(void) {
    if (present) return 0;

    pci_init();

    pci_device_t dev;
    if (!pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_LEGACY_ID, &dev)) {
        kprintf("virtio-blk: no disk attached\n");
        return -1;
    }

    uint32_t bar0 = pci_read32(&dev, PCI_BAR0);
    if (!(bar0 & 1)) {
        kprintf("virtio-blk: device has no legacy I/O port interface\n");
        return -1;
    }
    io_base = (uint16_t)(bar0 & ~3u);
    pci_enable_io_and_bus_master(&dev);

    // Handshake: reset, then announce ourselves step by step
    outb(io_base + REG_DEVICE_STATUS, 0);
    outb(io_base + REG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
    outb(io_base + REG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    // Feature negotiation: we use none of the optional features
    (void)inl(io_base + REG_DEVICE_FEATURES);
    outl(io_base + REG_DRIVER_FEATURES, 0);

    // Queue 0 is the request queue. The device dictates its size.
    outw(io_base + REG_QUEUE_SELECT, 0);
    queue_size = inw(io_base + REG_QUEUE_SIZE);
    if (queue_size < 3) {
        kprintf("virtio-blk: request queue too small (%u)\n", queue_size);
        outb(io_base + REG_DEVICE_STATUS, STATUS_FAILED);
        return -1;
    }

    // Layout fixed by the specification: descriptors, then the available
    // ring, then padding to a page boundary, then the used ring
    size_t avail_offset = sizeof(struct vring_desc) * queue_size;
    size_t used_offset = (avail_offset + sizeof(uint16_t) * (3 + queue_size) + QUEUE_ALIGN - 1) & ~(size_t)(QUEUE_ALIGN - 1);
    size_t total = used_offset + sizeof(uint16_t) * 3 + sizeof(struct vring_used_elem) * queue_size;
    size_t queue_pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    void *queue_phys = pmm_alloc_frames(queue_pages);
    void *header_phys = pmm_alloc_frame();
    void *data_phys = pmm_alloc_frame();
    if (!queue_phys || !header_phys || !data_phys || ((uint64_t)queue_phys >> 12) > 0xFFFFFFFFULL) {
        kprintf("virtio-blk: could not allocate queue memory\n");
        if (queue_phys) pmm_free_frames(queue_phys, queue_pages);
        if (header_phys) pmm_free_frame(header_phys);
        if (data_phys) pmm_free_frame(data_phys);
        outb(io_base + REG_DEVICE_STATUS, STATUS_FAILED);
        return -1;
    }

    uint64_t hhdm = pmm_get_hhdm_offset();
    uint8_t *queue = (uint8_t *)((uint64_t)queue_phys + hhdm);
    memset(queue, 0, queue_pages * PAGE_SIZE);
    desc = (struct vring_desc *)queue;
    avail = (struct vring_avail *)(queue + avail_offset);
    used = (volatile struct vring_used *)(queue + used_offset);
    last_used_idx = 0;

    req_header = (struct blk_request_header *)((uint64_t)header_phys + hhdm);
    req_header_phys = (uint64_t)header_phys;
    req_status = (volatile uint8_t *)req_header + 64;   // Same page, away from the header
    req_status_phys = req_header_phys + 64;
    req_data = (uint8_t *)((uint64_t)data_phys + hhdm);
    req_data_phys = (uint64_t)data_phys;

    outl(io_base + REG_QUEUE_ADDRESS, (uint32_t)((uint64_t)queue_phys >> 12));

    // The capacity is a 64-bit little-endian value in the device-specific area
    capacity_sectors = (uint64_t)inl(io_base + REG_CAPACITY) | ((uint64_t)inl(io_base + REG_CAPACITY + 4) << 32);

    // The device's interrupt arrives on the ISA IRQ named in its
    // configuration space
    uint8_t irq = pci_read8(&dev, PCI_INTERRUPT_LINE);
    if (irq > 0 && irq < 16) {
        register_irq_handler(irq, virtio_blk_irq);
        ioapic_route_isa_irq(irq);
    }

    outb(io_base + REG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);
    present = true;

    kprintf("virtio-blk: disk ready, %lu sectors (%lu MiB), queue size %u, IRQ %u\n",
            capacity_sectors, capacity_sectors / 2048, queue_size, irq);
    return 0;
}

bool virtio_blk_present(void) { return present; }
uint64_t virtio_blk_capacity(void) { return present ? capacity_sectors : 0; }
uint64_t virtio_blk_requests(void) { return completed_requests; }
uint64_t virtio_blk_interrupts(void) { return interrupt_count; }

// One request of up to SECTORS_PER_REQUEST sectors through the data page.
// Called with interrupts off and `busy` held.
static int do_request(uint32_t type, uint64_t lba, size_t count) {
    req_header->type = type;
    req_header->reserved = 0;
    req_header->sector = lba;
    *req_status = 0xFF; // Anything but 0: the device must overwrite it

    desc[0].addr = req_header_phys;
    desc[0].len = sizeof(struct blk_request_header);
    desc[0].flags = DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = req_data_phys;
    desc[1].len = (uint32_t)(count * VIRTIO_BLK_SECTOR_SIZE);
    desc[1].flags = DESC_F_NEXT | (type == BLK_T_IN ? DESC_F_WRITE : 0);
    desc[1].next = 2;

    desc[2].addr = req_status_phys;
    desc[2].len = 1;
    desc[2].flags = DESC_F_WRITE;
    desc[2].next = 0;

    // Publish the chain (it starts at descriptor 0), then tell the device.
    // The index must be written last: it is what makes the entry visible.
    avail->ring[avail->idx % queue_size] = 0;
    asm volatile("mfence" : : : "memory");
    avail->idx = (uint16_t)(avail->idx + 1);
    asm volatile("mfence" : : : "memory");
    outw(io_base + REG_QUEUE_NOTIFY, 0);

    // Sleep until the used ring moves. The interrupt handler wakes us; the
    // one-tick timeout makes this work even if the interrupt is lost, and
    // before the scheduler runs sched_block returns at once, so this loop
    // simply polls. Uninterruptible: the request must be seen through, or
    // the queue would be left in the middle of one.
    uint64_t deadline = pit_get_ticks() + 5 * TIMER_HZ;
    while (used->idx == last_used_idx) {
        if (pit_get_ticks() > deadline) {
            kprintf("virtio-blk: request timed out (sector %lu)\n", lba);
            return -1;
        }
        sched_block_uninterruptible(&used, pit_get_ticks() + 1);
    }
    last_used_idx = used->idx;
    completed_requests++;

    return (*req_status == 0) ? 0 : -1;
}

static int transfer(uint32_t type, uint64_t lba, void *buf, size_t count) {
    if (!present || !buf) return -1;
    if (count == 0) return 0;
    if (lba >= capacity_sectors || count > capacity_sectors - lba) return -1;

    uint8_t *bytes = buf;
    int result = 0;

    // One request at a time. Whoever finds the device busy sleeps until the
    // owner is done; these waits end with the request, so they need not be
    // interruptible, and must not be (see do_request).
    uint64_t flags = irq_save();
    while (busy) sched_block_uninterruptible(&busy, 0);
    busy = true;

    while (count > 0 && result == 0) {
        size_t chunk = count < SECTORS_PER_REQUEST ? count : SECTORS_PER_REQUEST;
        size_t chunk_bytes = chunk * VIRTIO_BLK_SECTOR_SIZE;

        if (type == BLK_T_OUT) memcpy(req_data, bytes, chunk_bytes);
        result = do_request(type, lba, chunk);
        if (result == 0 && type == BLK_T_IN) memcpy(bytes, req_data, chunk_bytes);

        bytes += chunk_bytes;
        lba += chunk;
        count -= chunk;
    }

    busy = false;
    sched_wakeup(&busy);
    irq_restore(flags);
    return result;
}

int virtio_blk_read(uint64_t lba, void *buf, size_t count) {
    return transfer(BLK_T_IN, lba, buf, count);
}

int virtio_blk_write(uint64_t lba, const void *buf, size_t count) {
    return transfer(BLK_T_OUT, lba, (void *)buf, count);
}
