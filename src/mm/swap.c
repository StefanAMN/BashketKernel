/**
 * @file swap.c
 * @brief Swapping pages out to disk and back (see swap.h)
 */

#include "swap.h"
#include "vmm.h"
#include "pmm.h"
#include "cow.h"
#include "heap.h"
#include "virtio_blk.h"
#include "mutex.h"
#include "spinlock.h"
#include "sched.h"
#include "kstring.h"
#include "kprintf.h"

#define SECTORS_PER_PAGE (PAGE_SIZE / VIRTIO_BLK_SECTOR_SIZE)

// The header mkswap writes (Linux, include/linux/swap.h, union swap_header):
// a kilobyte left free for a boot loader, then these fields, and the
// signature in the last ten bytes of the page
#define HEADER_VERSION     1024   // 32 bits: 1
#define HEADER_LAST_PAGE   1028   // 32 bits: number of the last usable page
#define HEADER_BAD_PAGES   1032   // 32 bits: how many pages are listed as unusable
#define SIGNATURE          "SWAPSPACE2"
#define SIGNATURE_LENGTH   10
#define MAX_SLOTS          (1u << 20)   // 4 GiB of swap: more than this kernel has a use for

static bool active = false;
static unsigned swap_disk;
static uint32_t slot_count;        // Slots are numbered 1 .. slot_count (0 is the header)
static uint16_t *slot_owners;      // Per slot: page-table entries that refer to it (0: free)
static uint32_t slots_free;
static uint32_t search_from = 1;   // Where the last free slot was found
static uint64_t pages_out, pages_in;

// Protects the slot table. Taken while the address space lock is held (an
// entry is dropped), so it ranks after it and nothing is called under it.
static spinlock_t slot_lock = SPINLOCK_RANKED("swap", LOCK_RANK_SWAP);

// One page goes out or comes in at a time. A sleeping lock: both wait for
// the disk.
static mutex_t swap_mutex = MUTEX_INIT("swap-io");

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool swap_parse_header(const uint8_t *page, uint64_t disk_pages, uint32_t *slots) {
    if (!page || !slots) return false;
    if (memcmp(page + PAGE_SIZE - SIGNATURE_LENGTH, SIGNATURE, SIGNATURE_LENGTH) != 0) return false;
    if (get32(page + HEADER_VERSION) != 1) return false;

    // A list of bad pages would have to be honoured. Disks have not needed
    // one for decades and mkswap no longer writes any: refuse rather than
    // ignore it.
    if (get32(page + HEADER_BAD_PAGES) != 0) return false;

    uint32_t last_page = get32(page + HEADER_LAST_PAGE);
    if (last_page == 0 || last_page >= disk_pages) return false;
    *slots = last_page > MAX_SLOTS ? MAX_SLOTS : last_page;
    return true;
}

int swap_init(unsigned disk) {
    static uint8_t header[PAGE_SIZE];
    if (active || disk >= virtio_blk_count()) return -1;
    if (virtio_blk_read_dev(disk, 0, header, SECTORS_PER_PAGE) != 0) return -1;

    uint32_t slots;
    if (!swap_parse_header(header, virtio_blk_capacity_of(disk) / SECTORS_PER_PAGE, &slots)) return -1;

    slot_owners = kmalloc(((size_t)slots + 1) * sizeof(uint16_t));
    if (!slot_owners) return -1;
    memset(slot_owners, 0, ((size_t)slots + 1) * sizeof(uint16_t));
    slot_count = slots;
    slots_free = slots;
    swap_disk = disk;
    active = true;
    kprintf("swap: disk %u, %u pages (%u MiB)\n", disk, slots, slots / 256);
    return 0;
}

bool swap_active(void) { return active; }
size_t swap_total_pages(void) { return active ? slot_count : 0; }
size_t swap_free_pages(void) { return active ? slots_free : 0; }
uint64_t swap_pages_out(void) { return pages_out; }
uint64_t swap_pages_in(void) { return pages_in; }

static uint32_t slot_alloc(void) {
    uint32_t found = 0;
    uint64_t flags = spinlock_acquire_irqsave(&slot_lock);
    for (uint32_t tried = 0; tried < slot_count && !found; tried++) {
        uint32_t slot = search_from;
        search_from = search_from == slot_count ? 1 : search_from + 1;
        if (slot_owners[slot] == 0) found = slot;
    }
    if (found) {
        slot_owners[found] = 1;
        slots_free--;
    }
    spinlock_release_irqrestore(&slot_lock, flags);
    return found; // 0: none free
}

void swap_slot_ref(uint32_t slot) {
    if (!active || slot == 0 || slot > slot_count) return;
    uint64_t flags = spinlock_acquire_irqsave(&slot_lock);
    if (slot_owners[slot] != 0 && slot_owners[slot] != 0xFFFF) slot_owners[slot]++;
    spinlock_release_irqrestore(&slot_lock, flags);
}

void swap_slot_unref(uint32_t slot) {
    // The number may come from a page table: nothing outside the table
    if (!active || slot == 0 || slot > slot_count) return;
    uint64_t flags = spinlock_acquire_irqsave(&slot_lock);
    if (slot_owners[slot] != 0 && --slot_owners[slot] == 0) slots_free++;
    spinlock_release_irqrestore(&slot_lock, flags);
}

size_t swap_out(uint64_t *space, uint64_t *cursor, size_t want) {
    if (!active || !space || !cursor) return 0;
    uint64_t hhdm = pmm_get_hhdm_offset();
    size_t done = 0;

    mutex_lock(&swap_mutex);
    while (done < want) {
        uint32_t slot = slot_alloc();
        if (slot == 0) break; // The swap disk is full

        // Take the page away first, then write it. From here on a touch of
        // it faults, and that fault waits for swap_mutex: nobody can change
        // the page while it is on its way out.
        uint64_t virt, frame;
        if (!vmm_swap_pick(space, cursor, slot, &virt, &frame)) {
            swap_slot_unref(slot);
            break;            // Nothing left that may go
        }
        if (virtio_blk_write_dev(swap_disk, (uint64_t)slot * SECTORS_PER_PAGE, (void *)(frame + hhdm), SECTORS_PER_PAGE) != 0) {
            vmm_swap_install(space, virt, frame); // Put it back as it was
            swap_slot_unref(slot);
            kprintf("swap: write error, page kept in memory\n");
            break;
        }
        pmm_unref_frame((void *)frame);
        pages_out++;
        done++;
    }
    mutex_unlock(&swap_mutex);
    return done;
}

int swap_in(uint64_t *space, uint64_t virt) {
    if (!active || !space) return 0;
    virt &= ~(uint64_t)(PAGE_SIZE - 1);
    if (!vmm_pte_swapped(vmm_get_pte(space, virt))) return 0;

    int result = -1;
    mutex_lock(&swap_mutex);
    uint64_t pte = vmm_get_pte(space, virt);
    if (!vmm_pte_swapped(pte)) {
        result = 1;           // Came back while we waited for the lock
    } else {
        uint32_t slot = vmm_pte_swap_slot(pte);
        void *frame = slot >= 1 && slot <= slot_count ? pmm_alloc_frame() : NULL;
        if (frame) {
            void *view = (void *)((uint64_t)frame + pmm_get_hhdm_offset());
            if (virtio_blk_read_dev(swap_disk, (uint64_t)slot * SECTORS_PER_PAGE, view, SECTORS_PER_PAGE) == 0 &&
                vmm_swap_install(space, virt, (uint64_t)frame)) {
                swap_slot_unref(slot);
                pages_in++;
                result = 1;
            } else {
                pmm_free_frame(frame);
            }
        }
    }
    mutex_unlock(&swap_mutex);
    return result;
}

void swap_relieve_pressure(void) {
    if (!active) return;
    task_t *task = sched_get_current_task();
    if (!task || !task->is_user) return;

    size_t free_frames = pmm_get_free_frames();
    if (free_frames >= SWAP_LOW_FRAMES) return;

    // A process may have nothing it can give up: just after a fork every
    // page it has is still shared with its parent. Finding that out means
    // looking at its whole address space, which must not happen at every
    // fault: after a search that found nothing, the next one waits until
    // half of what was free then has gone.
    if (task->swap_retry_below != 0 && free_frames >= task->swap_retry_below) return;

    size_t want = SWAP_HIGH_FRAMES - free_frames;
    size_t got = swap_out(vmm_space_from_phys((uint64_t)task->pml4_phys), &task->swap_cursor, want);
    task->swap_retry_below = got < want ? free_frames / 2 + 1 : 0;
}
