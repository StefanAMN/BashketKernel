/**
 * @file virtio_blk.c
 * @brief virtio block device (disk) driver, legacy PCI interface
 *
 * A disk request is a chain of three buffers on the device's one virtqueue
 * (see virtqueue.h for how those work):
 *     1. a 16-byte header: read or write, and the sector number
 *     2. the data: 512 bytes per sector
 *     3. one status byte, which the device sets to 0 on success
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
#include "virtqueue.h"

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_BLK_LEGACY_ID   0x1001

#define REG_CAPACITY VIRTIO_REG_DEVICE_CONFIG // 64 bits: disk size in sectors

#define BLK_T_IN  0 // Read from the disk
#define BLK_T_OUT 1 // Write to the disk

#define SECTORS_PER_REQUEST (PAGE_SIZE / VIRTIO_BLK_SECTOR_SIZE) // The data buffer is one page

struct blk_request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static bool present = false;
static uint16_t io_base;
static uint64_t capacity_sectors;
static virtqueue_t queue;            // The request queue

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
    uint8_t isr = inb(io_base + VIRTIO_REG_ISR_STATUS);
    if (isr & 1) {
        interrupt_count++;
        sched_wakeup(&queue);
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
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, 0);
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Feature negotiation: we use none of the optional features
    (void)inl(io_base + VIRTIO_REG_DEVICE_FEATURES);
    outl(io_base + VIRTIO_REG_DRIVER_FEATURES, 0);

    // Queue 0 is the request queue; plus one page for the request header and
    // status byte and one for the data
    void *header_phys = pmm_alloc_frame();
    void *data_phys = pmm_alloc_frame();
    if (!header_phys || !data_phys || !virtqueue_setup(&queue, io_base, 0) || queue.size < 3) {
        kprintf("virtio-blk: could not set up the request queue\n");
        if (header_phys) pmm_free_frame(header_phys);
        if (data_phys) pmm_free_frame(data_phys);
        outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    uint64_t hhdm = pmm_get_hhdm_offset();
    req_header = (struct blk_request_header *)((uint64_t)header_phys + hhdm);
    req_header_phys = (uint64_t)header_phys;
    req_status = (volatile uint8_t *)req_header + 64;   // Same page, away from the header
    req_status_phys = req_header_phys + 64;
    req_data = (uint8_t *)((uint64_t)data_phys + hhdm);
    req_data_phys = (uint64_t)data_phys;

    // The capacity is a 64-bit little-endian value in the device-specific area
    capacity_sectors = (uint64_t)inl(io_base + REG_CAPACITY) | ((uint64_t)inl(io_base + REG_CAPACITY + 4) << 32);

    // The device's interrupt arrives on the ISA IRQ named in its
    // configuration space
    uint8_t irq = pci_read8(&dev, PCI_INTERRUPT_LINE);
    if (irq > 0 && irq < 16) {
        register_irq_handler(irq, virtio_blk_irq);
        ioapic_route_isa_irq(irq);
    }

    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    present = true;

    kprintf("virtio-blk: disk ready, %lu sectors (%lu MiB), queue size %u, IRQ %u\n",
            capacity_sectors, capacity_sectors / 2048, queue.size, irq);
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

    // Header (the device reads it), data (read or written, depending on the
    // direction), status (the device writes it)
    virtqueue_buffer_t buffers[3] = {
        { req_header_phys, sizeof(struct blk_request_header) },
        { req_data_phys, (uint32_t)(count * VIRTIO_BLK_SECTOR_SIZE) },
        { req_status_phys, 1 },
    };
    unsigned out_count = (type == BLK_T_OUT) ? 2 : 1;
    if (virtqueue_add(&queue, buffers, out_count, 3 - out_count) < 0) return -1;
    virtqueue_kick(&queue);

    // Sleep until the used ring moves. The interrupt handler wakes us; the
    // one-tick timeout makes this work even if the interrupt is lost, and
    // before the scheduler runs sched_block returns at once, so this loop
    // simply polls. Uninterruptible: the request must be seen through, or
    // the queue would be left in the middle of one.
    uint64_t deadline = pit_get_ticks() + 5 * TIMER_HZ;
    while (!virtqueue_has_used(&queue)) {
        if (pit_get_ticks() > deadline) {
            kprintf("virtio-blk: request timed out (sector %lu)\n", lba);
            return -1;
        }
        sched_block_uninterruptible(&queue, pit_get_ticks() + 1);
    }
    virtqueue_collect(&queue, NULL, NULL);
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
