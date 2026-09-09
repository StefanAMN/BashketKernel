/**
 * @file pmm.c
 * @brief Implementation of Physical Memory Manager Frame Allocator
 */

#include "pmm.h"
#include "kprintf.h"
#include "spinlock.h"

static uint8_t *bitmap = NULL;
static size_t total_frames = 0;
static size_t free_frames = 0;
static size_t used_frames = 0;
static uint64_t hhdm_off = 0;

// Frame where the next search starts. Everything below it was in use the last
// time we looked, so allocation does not rescan the bitmap from the beginning.
static size_t next_free_hint = 1;

// The bitmap is shared by every task and by interrupt context.
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline void bitmap_set(size_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(size_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool bitmap_test(size_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

uint64_t pmm_get_hhdm_offset(void) { return hhdm_off; }
size_t pmm_get_total_frames(void) { return total_frames; }
size_t pmm_get_free_frames(void)  { return free_frames; }
size_t pmm_get_used_frames(void)  { return used_frames; }

// RAM that exists and that we could ever hand out. Reserved and MMIO entries
// are excluded on purpose: firmware reports reserved regions far above the end
// of RAM (around 1 TiB on QEMU), and sizing the bitmap from those made a 2 GiB
// machine look like it had 1 TiB.
static bool entry_is_ram(const struct limine_memmap_entry *entry) {
    return entry->type == LIMINE_MEMMAP_USABLE ||
           entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    hhdm_off = hhdm_offset;
    uint64_t highest_addr = 0;

    // Determine the highest RAM address to size the bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (!entry_is_ram(entry)) continue;

        uint64_t top = entry->base + entry->length;
        if (top > highest_addr) {
            highest_addr = top;
        }
    }

    total_frames = highest_addr / PAGE_SIZE;
    size_t bitmap_size = (total_frames + 7) / 8;

    // Find usable RAM region to store the bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap = (uint8_t *)(entry->base + hhdm_off);
            break;
        }
    }

    if (bitmap == NULL) {
        kprintf("PMM ERROR: Could not find usable memory for bitmap!\n");
        return;
    }

    // Set all frames to used initially (0xFF)
    for (size_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }
    used_frames = total_frames;
    free_frames = 0;

    // Mark USABLE RAM regions as free in bitmap.
    //
    // Nothing else needs reserving by hand. The kernel image, the boot stack
    // and the page tables Limine built are not in USABLE entries (they are
    // EXECUTABLE_AND_MODULES and BOOTLOADER_RECLAIMABLE), so they stay marked
    // used from the loop above.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            size_t start_frame = entry->base / PAGE_SIZE;
            size_t frame_count = entry->length / PAGE_SIZE;

            for (size_t f = 0; f < frame_count; f++) {
                size_t frame = start_frame + f;
                if (frame < total_frames && bitmap_test(frame)) {
                    bitmap_clear(frame);
                    used_frames--;
                    free_frames++;
                }
            }
        }
    }

    // Mark bitmap's own physical frames as used
    uint64_t bitmap_phys = (uint64_t)bitmap - hhdm_off;
    size_t bitmap_start_frame = bitmap_phys / PAGE_SIZE;
    size_t bitmap_frame_count = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t f = 0; f < bitmap_frame_count; f++) {
        size_t frame = bitmap_start_frame + f;
        if (frame < total_frames && !bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
            free_frames--;
        }
    }

    // Frame 0 is reserved so that physical address 0 can mean "no frame"
    if (total_frames > 0 && !bitmap_test(0)) {
        bitmap_set(0);
        used_frames++;
        free_frames--;
    }

    next_free_hint = 1;

    kprintf("PMM Init: Total Frames: %zu (%zu MB), Free Frames: %zu (%zu MB)\n",
            total_frames, (total_frames * PAGE_SIZE) / (1024 * 1024),
            free_frames, (free_frames * PAGE_SIZE) / (1024 * 1024));
}

// Search [from, to) for `count` consecutive free frames. Returns the first
// frame of the run, or 0 if there is none (frame 0 is never allocatable).
static size_t find_free_run(size_t from, size_t to, size_t count) {
    size_t consecutive = 0;
    size_t start_frame = 0;

    for (size_t i = from; i < to; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) {
                start_frame = i;
            }
            consecutive++;

            if (consecutive == count) {
                return start_frame;
            }
        } else {
            consecutive = 0;
        }
    }

    return 0;
}

void *pmm_alloc_frames(size_t count) {
    if (count == 0 || bitmap == NULL) return NULL;

    uint64_t flags = spinlock_acquire_irqsave(&pmm_lock);

    // Start at the hint, then wrap around once to cover the frames below it
    size_t start_frame = find_free_run(next_free_hint, total_frames, count);
    if (start_frame == 0) {
        start_frame = find_free_run(1, total_frames, count);
    }

    if (start_frame == 0) {
        spinlock_release_irqrestore(&pmm_lock, flags);
        kprintf("PMM ERROR: Out of physical memory for %zu frames!\n", count);
        return NULL;
    }

    for (size_t f = 0; f < count; f++) {
        bitmap_set(start_frame + f);
    }
    used_frames += count;
    free_frames -= count;
    next_free_hint = start_frame + count;

    spinlock_release_irqrestore(&pmm_lock, flags);
    return (void *)(start_frame * PAGE_SIZE);
}

void *pmm_alloc_frame(void) {
    return pmm_alloc_frames(1);
}

void pmm_free_frames(void *phys_addr, size_t count) {
    if (phys_addr == NULL || count == 0 || bitmap == NULL) return;

    size_t start_frame = (uint64_t)phys_addr / PAGE_SIZE;
    size_t bad_frame = 0;

    uint64_t flags = spinlock_acquire_irqsave(&pmm_lock);

    for (size_t f = 0; f < count; f++) {
        size_t frame = start_frame + f;
        if (frame == 0 || frame >= total_frames || !bitmap_test(frame)) {
            // Out of range, or already free: a double free or a bad pointer.
            // Remember it and report after the lock is dropped.
            if (bad_frame == 0) bad_frame = frame ? frame : (size_t)-1;
            continue;
        }

        bitmap_clear(frame);
        used_frames--;
        free_frames++;
        if (frame < next_free_hint) {
            next_free_hint = frame;
        }
    }

    spinlock_release_irqrestore(&pmm_lock, flags);

    if (bad_frame != 0) {
        kprintf("PMM WARNING: Freeing frame %zu which is not allocated (double free?)\n",
                bad_frame == (size_t)-1 ? (size_t)0 : bad_frame);
    }
}

void pmm_free_frame(void *phys_addr) {
    pmm_free_frames(phys_addr, 1);
}
