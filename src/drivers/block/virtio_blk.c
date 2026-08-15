/**
 * @file virtio_blk.c
 * @brief virtio block device (disk) driver
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



#define CONFIG_CAPACITY 0 // Device configuration: 64 bits, the disk size in sectors

#define BLK_T_IN  0 // Read from the disk
#define BLK_T_OUT 1 // Write to the disk

#define SECTORS_PER_REQUEST (PAGE_SIZE / VIRTIO_BLK_SECTOR_SIZE) // The data buffer is one page

struct blk_request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

// Several requests can be with a device at once: each uses a slot, which
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

// Everything about one disk. There may be more than one: each has its own
// I/O ports, queue, slots and lock, and nothing below is shared.
typedef struct {
    bool present;
    virtio_device_t virtio;          // Its registers, through whichever interface it has
    uint64_t capacity_sectors;
    virtqueue_t queue;               // The request queue
    blk_slot_t slots[BLK_SLOTS];
    uint8_t slot_of_request[MAX_QUEUE_SIZE]; // Which slot a request (named by its first descriptor) belongs to
    uint32_t in_flight, peak_in_flight;
    uint64_t completed_requests;
    volatile uint64_t interrupt_count;
    spinlock_t lock;                 // Protects the queue, the slots and the counters
} blk_device_t;

static blk_device_t devices[VIRTIO_BLK_MAX_DEVICES];
static unsigned device_count = 0;

// Take every finished request off the d->queue and wake its owner. Caller
// holds disk_lock.
static void collect_finished(blk_device_t *d) {
    uint16_t id;
    while (virtqueue_collect(&d->queue, &id, NULL)) {
        // The id comes from the device: check it before using it
        if (id >= MAX_QUEUE_SIZE || d->slot_of_request[id] >= BLK_SLOTS) continue;
        blk_slot_t *slot = &d->slots[d->slot_of_request[id]];
        if (!slot->in_use || slot->done) continue;
        slot->done = true;
        d->completed_requests++;
        sched_wakeup(slot);
    }
}

// The disks' interrupt handler (every disk on a line shares it)
static void virtio_blk_irq(struct registers *regs) {
    (void)regs;
    // Interrupt lines are shared, and so is this handler: ask every disk.
    // Reading the ISR register is what makes a device lower its line; bit 0
    // set means its used ring changed.
    for (unsigned i = 0; i < device_count; i++) {
        blk_device_t *d = &devices[i];
        uint8_t isr = virtio_isr(&d->virtio);
        if (!(isr & 1)) continue;
        // Under the lock, so a requester can not be between "not done yet"
        // and "asleep" when this arrives
        uint64_t flags = spinlock_acquire_irqsave(&d->lock);
        d->interrupt_count++;
        random_add_event(d->interrupt_count);
        collect_finished(d);
        spinlock_release_irqrestore(&d->lock, flags);
    }
}

// Set disk `number` up from its PCI device: interface, features, request
// queue, the slots' DMA pages, its size; register and route its interrupt
// line unless that was just done for the previous disk. Returns 0 or -1
// (everything it took is given back).
static int init_device(blk_device_t *d, const pci_device_t *pci, unsigned number, bool *irq_registered, uint8_t *registered_irq) {
    pci_device_t dev = *pci;
    spinlock_init_ranked(&d->lock, "virtio-blk", LOCK_RANK_DEVICE);

    // Reset, announce a driver, and agree on features: we use none of the
    // optional ones
    if (!virtio_open(&d->virtio, &dev) || !virtio_negotiate(&d->virtio, 0, NULL)) {
        kprintf("virtio-blk: device offers no interface this kernel can use\n");
        return -1;
    }

    // Queue 0 is the request d->queue; each slot needs three descriptors
    // (header, data, status)
    bool ok = virtqueue_setup(&d->queue, &d->virtio, 0) && d->queue.size >= 3 && d->queue.size <= MAX_QUEUE_SIZE;
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
        d->slots[i].header = (struct blk_request_header *)((uint64_t)header_phys + hhdm);
        d->slots[i].header_phys = (uint64_t)header_phys;
        d->slots[i].status = (volatile uint8_t *)d->slots[i].header + 64;   // Same page, away from the header
        d->slots[i].status_phys = d->slots[i].header_phys + 64;
        d->slots[i].data = (uint8_t *)((uint64_t)data_phys + hhdm);
        d->slots[i].data_phys = (uint64_t)data_phys;
    }
    if (!ok) {
        kprintf("virtio-blk: could not set up the request d->queue\n");
        for (int i = 0; i < BLK_SLOTS; i++) {
            if (d->slots[i].header_phys) pmm_free_frame((void *)d->slots[i].header_phys);
            if (d->slots[i].data_phys) pmm_free_frame((void *)d->slots[i].data_phys);
        }
        memset(d->slots, 0, sizeof(d->slots));
        virtio_fail(&d->virtio);
        return -1;
    }

    d->capacity_sectors = virtio_config64(&d->virtio, CONFIG_CAPACITY);

    // The device's interrupt arrives on the ISA IRQ named in its
    // configuration space
    uint8_t irq = d->virtio.irq;
    if (irq > 0 && irq < 16 && !(*irq_registered && *registered_irq == irq)) {
        register_irq_handler(irq, virtio_blk_irq); // One handler serves every disk on the line
        ioapic_route_isa_irq(irq);
        *irq_registered = true;
        *registered_irq = irq;
    }

    virtio_driver_ok(&d->virtio);
    d->present = true;

    kprintf("virtio-blk: disk %u ready, %lu sectors (%lu MiB), queue size %u, IRQ %u, %s interface\n",
            number, d->capacity_sectors, d->capacity_sectors / 2048, d->queue.size, irq, virtio_interface_name(&d->virtio));
    return 0;
}

int virtio_blk_init(void) {
    if (device_count > 0) return 0;
    pci_init();

    bool irq_registered = false;
    uint8_t registered_irq = 0;
    for (unsigned n = 0; device_count < VIRTIO_BLK_MAX_DEVICES; n++) {
        pci_device_t dev;
        if (!virtio_pci_find(VIRTIO_TYPE_BLOCK, n, &dev)) break;
        // (The handler walks devices[0 .. device_count): count it only once
        // it is set up, and set it up before its interrupt can be routed.)
        if (init_device(&devices[device_count], &dev, device_count, &irq_registered, &registered_irq) == 0) device_count++;
    }
    if (device_count == 0) {
        kprintf("virtio-blk: no disk attached\n");
        return -1;
    }
    return 0;
}

unsigned virtio_blk_count(void) { return device_count; }
bool virtio_blk_present(void) { return device_count > 0; }
uint64_t virtio_blk_capacity(void) { return virtio_blk_capacity_of(0); }
uint64_t virtio_blk_capacity_of(unsigned dev) { return dev < device_count ? devices[dev].capacity_sectors : 0; }

uint64_t virtio_blk_requests(unsigned dev) { return dev < device_count ? devices[dev].completed_requests : 0; }
uint64_t virtio_blk_interrupts(unsigned dev) { return dev < device_count ? devices[dev].interrupt_count : 0; }

uint32_t virtio_blk_peak_in_flight(void) {
    uint32_t peak = 0;
    for (unsigned i = 0; i < device_count; i++) if (devices[i].peak_in_flight > peak) peak = devices[i].peak_in_flight;
    return peak;
}

// One request of up to SECTORS_PER_REQUEST sectors, from start to finish.
static int do_request(blk_device_t *d, uint32_t type, uint64_t lba, uint8_t *bytes, size_t count) {
    size_t byte_count = count * VIRTIO_BLK_SECTOR_SIZE;

    // A slot of our own. These waits end when a request does, so they need
    // not be interruptible, and the ones below must not be: a task killed
    // while the device still has its request would leave the slot in use
    // for ever.
    uint64_t flags = spinlock_acquire_irqsave(&d->lock);
    blk_slot_t *slot = NULL;
    for (;;) {
        for (int i = 0; i < BLK_SLOTS && !slot; i++) {
            if (!d->slots[i].in_use) slot = &d->slots[i];
        }
        if (slot) break;
        sched_sleep_uninterruptible(d->slots, &d->lock, 0);
    }
    slot->in_use = true;
    slot->done = false;
    spinlock_release_irqrestore(&d->lock, flags);

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
    flags = spinlock_acquire_irqsave(&d->lock);
    int id = virtqueue_add(&d->queue, buffers, out_count, 3 - out_count);
    if (id >= 0 && id < MAX_QUEUE_SIZE) {
        d->slot_of_request[id] = (uint8_t)(slot - d->slots);
        if (++d->in_flight > d->peak_in_flight) d->peak_in_flight = d->in_flight;
        virtqueue_kick(&d->queue);

        // Sleep until the device has answered. The interrupt handler wakes
        // us; the one-tick timeout makes this work even if an interrupt is
        // lost, and before the scheduler runs sched_sleep returns at once,
        // so this loop simply polls.
        uint64_t deadline = timer_ticks() + 5 * TIMER_HZ;
        while (!slot->done && timer_ticks() <= deadline) {
            collect_finished(d);
            if (!slot->done) sched_sleep_uninterruptible(slot, &d->lock, timer_ticks() + 1);
        }
        d->in_flight--;
        if (slot->done) result = (*slot->status == 0) ? 0 : -1;
        else kprintf("virtio-blk: request timed out (sector %lu)\n", lba);
    }
    bool answered = slot->done;
    spinlock_release_irqrestore(&d->lock, flags);

    if (result == 0 && type == BLK_T_IN) memcpy(bytes, slot->data, byte_count);

    // A slot whose request the device never answered is not handed out
    // again: the device may still write into its pages
    flags = spinlock_acquire_irqsave(&d->lock);
    if (answered || id < 0) {
        slot->in_use = false;
        sched_wakeup(d->slots);
    }
    spinlock_release_irqrestore(&d->lock, flags);
    return result;
}

// Check a request against the disk's size and carry it out in pieces of at
// most one page. Returns 0, or -1 at the first piece that fails.
static int transfer(unsigned dev, uint32_t type, uint64_t lba, void *buf, size_t count) {
    if (dev >= device_count || !buf) return -1;
    blk_device_t *d = &devices[dev];
    if (count == 0) return 0;
    if (lba >= d->capacity_sectors || count > d->capacity_sectors - lba) return -1;

    uint8_t *bytes = buf;
    int result = 0;
    while (count > 0 && result == 0) {
        size_t chunk = count < SECTORS_PER_REQUEST ? count : SECTORS_PER_REQUEST;
        result = do_request(d, type, lba, bytes, chunk);
        bytes += chunk * VIRTIO_BLK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return result;
}

// The four entry points: see virtio_blk.h
int virtio_blk_read(uint64_t lba, void *buf, size_t count) {
    return transfer(0, BLK_T_IN, lba, buf, count);
}

int virtio_blk_write(uint64_t lba, const void *buf, size_t count) {
    return transfer(0, BLK_T_OUT, lba, (void *)buf, count);
}

int virtio_blk_read_dev(unsigned dev, uint64_t lba, void *buf, size_t count) {
    return transfer(dev, BLK_T_IN, lba, buf, count);
}

int virtio_blk_write_dev(unsigned dev, uint64_t lba, const void *buf, size_t count) {
    return transfer(dev, BLK_T_OUT, lba, (void *)buf, count);
}
