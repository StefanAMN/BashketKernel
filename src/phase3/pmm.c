/**
 * @file pmm.c
 * @brief Implementation of Physical Memory Manager Frame Allocator
 */

#include "pmm.h"
#include "kprintf.h"

static uint8_t *bitmap = NULL;
static size_t total_frames = 0;
static size_t free_frames = 0;
static size_t used_frames = 0;
static uint64_t hhdm_off = 0;

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

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    hhdm_off = hhdm_offset;
    uint64_t highest_addr = 0;

    // Determine highest physical memory address to size the bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
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

    // Mark USABLE RAM regions as free in bitmap
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

    // Frame 0 is reserved
    if (!bitmap_test(0)) {
        bitmap_set(0);
        used_frames++;
        free_frames--;
    }

    // Reserve physical frames containing the active boot stack pointer
    uint64_t current_rsp;
    asm volatile("mov %0, rsp" : "=r"(current_rsp));
    uint64_t stack_phys = (current_rsp >= hhdm_off) ? (current_rsp - hhdm_off) : (current_rsp & 0x7FFFFFFF);
    size_t stack_start_frame = (stack_phys / PAGE_SIZE) > 16 ? (stack_phys / PAGE_SIZE) - 16 : 0;
    size_t stack_end_frame = (stack_phys / PAGE_SIZE) + 16;

    for (size_t f = stack_start_frame; f <= stack_end_frame && f < total_frames; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
            free_frames--;
        }
    }

    // Reserve physical frames containing kernel text/rodata/data/bss sections
    extern char __kernel_start[];
    extern char __kernel_end[];
    uint64_t kstart_phys = (uint64_t)__kernel_start >= hhdm_off ? ((uint64_t)__kernel_start - hhdm_off) : ((uint64_t)__kernel_start & 0x7FFFFFFF);
    uint64_t kend_phys = (uint64_t)__kernel_end >= hhdm_off ? ((uint64_t)__kernel_end - hhdm_off) : ((uint64_t)__kernel_end & 0x7FFFFFFF);

    size_t kstart_frame = kstart_phys / PAGE_SIZE;
    size_t kend_frame = (kend_phys + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t f = kstart_frame; f <= kend_frame && f < total_frames; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
            free_frames--;
        }
    }

    kprintf("PMM Init: Total Frames: %zu (%zu MB), Free Frames: %zu (%zu MB)\n",
            total_frames, (total_frames * PAGE_SIZE) / (1024 * 1024),
            free_frames, (free_frames * PAGE_SIZE) / (1024 * 1024));
}

void *pmm_alloc_frames(size_t count) {
    if (count == 0) return NULL;

    size_t consecutive = 0;
    size_t start_frame = 0;

    for (size_t i = 1; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) {
                start_frame = i;
            }
            consecutive++;

            if (consecutive == count) {
                for (size_t f = 0; f < count; f++) {
                    bitmap_set(start_frame + f);
                }
                used_frames += count;
                free_frames -= count;
                return (void *)(start_frame * PAGE_SIZE);
            }
        } else {
            consecutive = 0;
        }
    }

    kprintf("PMM ERROR: Out of physical memory for %zu frames!\n", count);
    return NULL;
}

void *pmm_alloc_frame(void) {
    return pmm_alloc_frames(1);
}

void pmm_free_frames(void *phys_addr, size_t count) {
    if (phys_addr == NULL || count == 0) return;

    size_t start_frame = (uint64_t)phys_addr / PAGE_SIZE;

    for (size_t f = 0; f < count; f++) {
        size_t frame = start_frame + f;
        if (frame < total_frames && bitmap_test(frame)) {
            bitmap_clear(frame);
            used_frames--;
            free_frames++;
        }
    }
}

void pmm_free_frame(void *phys_addr) {
    pmm_free_frames(phys_addr, 1);
}
