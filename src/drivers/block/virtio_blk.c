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
#include "random.h"
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

// Several requests can be with the device at once: each uses a slot, which
// owns the memory the device reads and writes directly (DMA): a page with
// the request header and the status byte, and a page of data. A task that
// finds every slot taken sleeps until one is given back.
#define BLK_SLOTS 8
#define MAX_QUEUE_SIZE 1024          // Size of the descriptor-to-slot table

typedef struct {
    bool in_use;                     // Claimed by a task
    bool done;                       // The device has answered
    struct blk_request_header *header;
    volatile uint8_t *status;
    uint8_t *data;
    uint64_t header_phys, status_phys, data_phys;
} blk_slot_t;

static blk_slot_t slots[BLK_SLOTS];
static uint8_t slot_of_request[MAX_QUEUE_SIZE]; // Which slot a request (named by its first descriptor) belongs to
static uint32_t in_flight = 0, peak_in_flight = 0;
static uint64_t completed_requests = 0;
static volatile uint64_t interrupt_count = 0;

// Protects the queue, the slots and the counters
static spinlock_t disk_lock = SPINLOCK_RANKED("virtio-blk", LOCK_RANK_DEVICE);

// Take every finished request off the queue and wake its owner. Caller
// holds disk_lock.
static void collect_finished(void) {
    uint16_t id;
    while (virtqueue_collect(&queue, &id, NULL)) {
        // The id comes from the device: check it before using it
        if (id >= MAX_QUEUE_SIZE || slot_of_request[id] >= BLK_SLOTS) continue;
        blk_slot_t *slot = &slots[slot_of_request[id]];
        if (!slot->in_use || slot->done) continue;
        slot->done = true;
        completed_requests++;
        sched_wakeup(slot);
    }
}

static void virtio_blk_irq(struct registers *regs) {
    (void)regs;
    // Reading the ISR register is what makes the device lower its interrupt
    // line. Bit 0 set means the used ring changed.
    uint8_t isr = inb(io_base + VIRTIO_REG_ISR_STATUS);
    if (isr & 1) {
        // Under the lock, so a requester can not be between "not done yet"
        // and "asleep" when this arrives
        uint64_t flags = spinlock_acquire_irqsave(&disk_lock);
        interrupt_count++;
        random_add_event(interrupt_count);
        collect_finished();
        spinlock_release_irqrestore(&disk_lock, flags);
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

    // Queue 0 is the request queue; each slot needs three descriptors
    // (header, data, status)
    bool ok = virtqueue_setup(&queue, io_base, 0) && queue.size >= 3 && queue.size <= MAX_QUEUE_SIZE;
    uint64_t hhdm = pmm_get_hhdm_offset();
    for (int i = 0; ok && i < BLK_SLOTS; i++) {
        void *header_phys = pmm_alloc_frame();
        void *data_phys = pmm_alloc_frame();
        if (!header_phys || !data_phys) {
            if (header_phys) pmm_free_frame(header_phys);
            if (data_phys) pmm_free_frame(data_phys);
            ok = false;
            break;
        }
        slots[i].header = (struct blk_request_header *)((uint64_t)header_phys + hhdm);
        slots[i].header_phys = (uint64_t)header_phys;
        slots[i].status = (volatile uint8_t *)slots[i].header + 64;   // Same page, away from the header
        slots[i].status_phys = slots[i].header_phys + 64;
        slots[i].data = (uint8_t *)((uint64_t)data_phys + hhdm);
        slots[i].data_phys = (uint64_t)data_phys;
    }
    if (!ok) {
        kprintf("virtio-blk: could not set up the request queue\n");
        for (int i = 0; i < BLK_SLOTS; i++) {
            if (slots[i].header_phys) pmm_free_frame((void *)slots[i].header_phys);
            if (slots[i].data_phys) pmm_free_frame((void *)slots[i].data_phys);
        }
        memset(slots, 0, sizeof(slots));
        outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

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

uint32_t virtio_blk_peak_in_flight(void) { return peak_in_flight; }

// One request of up to SECTORS_PER_REQUEST sectors, from start to finish.
static int do_request(uint32_t type, uint64_t lba, uint8_t *bytes, size_t count) {
    size_t byte_count = count * VIRTIO_BLK_SECTOR_SIZE;

    // A slot of our own. These waits end when a request does, so they need
    // not be interruptible, and the ones below must not be: a task killed
    // while the device still has its request would leave the slot in use
    // for ever.
    uint64_t flags = spinlock_acquire_irqsave(&disk_lock);
    blk_slot_t *slot = NULL;
    for (;;) {
        for (int i = 0; i < BLK_SLOTS && !slot; i++) {
            if (!slots[i].in_use) slot = &slots[i];
        }
        if (slot) break;
        sched_sleep_uninterruptible(slots, &disk_lock, 0);
    }
    slot->in_use = true;
    slot->done = false;
    spinlock_release_irqrestore(&disk_lock, flags);

    // The slot is ours alone: fill it without the lock
    slot->header->type = type;
    slot->header->reserved = 0;
    slot->header->sector = lba;
    *slot->status = 0xFF; // Anything but 0: the device must overwrite it
    if (type == BLK_T_OUT) memcpy(slot->data, bytes, byte_count);

    // Header (the device reads it), data (read or written, depending on the
    // direction), status (the device writes it)
    virtqueue_buffer_t buffers[3] = {
        { slot->header_phys, sizeof(struct blk_request_header) },
        { slot->data_phys, (uint32_t)byte_count },
        { slot->status_phys, 1 },
    };
    unsigned out_count = (type == BLK_T_OUT) ? 2 : 1;

    int result = -1;
    flags = spinlock_acquire_irqsave(&disk_lock);
    int id = virtqueue_add(&queue, buffers, out_count, 3 - out_count);
    if (id >= 0 && id < MAX_QUEUE_SIZE) {
        slot_of_request[id] = (uint8_t)(slot - slots);
        if (++in_flight > peak_in_flight) peak_in_flight = in_flight;
        virtqueue_kick(&queue);

        // Sleep until the device has answered. The interrupt handler wakes
        // us; the one-tick timeout makes this work even if an interrupt is
        // lost, and before the scheduler runs sched_sleep returns at once,
        // so this loop simply polls.
        uint64_t deadline = pit_get_ticks() + 5 * TIMER_HZ;
        while (!slot->done && pit_get_ticks() <= deadline) {
            collect_finished();
            if (!slot->done) sched_sleep_uninterruptible(slot, &disk_lock, pit_get_ticks() + 1);
        }
        in_flight--;
        if (slot->done) result = (*slot->status == 0) ? 0 : -1;
        else kprintf("virtio-blk: request timed out (sector %lu)\n", lba);
    }
    bool answered = slot->done;
    spinlock_release_irqrestore(&disk_lock, flags);

    if (result == 0 && type == BLK_T_IN) memcpy(bytes, slot->data, byte_count);

    // A slot whose request the device never answered is not handed out
    // again: the device may still write into its pages
    flags = spinlock_acquire_irqsave(&disk_lock);
    if (answered || id < 0) {
        slot->in_use = false;
        sched_wakeup(slots);
    }
    spinlock_release_irqrestore(&disk_lock, flags);
    return result;
}

static int transfer(uint32_t type, uint64_t lba, void *buf, size_t count) {
    if (!present || !buf) return -1;
    if (count == 0) return 0;
    if (lba >= capacity_sectors || count > capacity_sectors - lba) return -1;

    uint8_t *bytes = buf;
    int result = 0;
    while (count > 0 && result == 0) {
        size_t chunk = count < SECTORS_PER_REQUEST ? count : SECTORS_PER_REQUEST;
        result = do_request(type, lba, bytes, chunk);
        bytes += chunk * VIRTIO_BLK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return result;
}

int virtio_blk_read(uint64_t lba, void *buf, size_t count) {
    return transfer(BLK_T_IN, lba, buf, count);
}

int virtio_blk_write(uint64_t lba, const void *buf, size_t count) {
    return transfer(BLK_T_OUT, lba, (void *)buf, count);
}
