/**
 * @file pmm.h
 * @brief Physical Memory Manager (PMM) Bitmap Allocator Interface
 * 
 * Manages physical memory frames (4096 bytes / 4KiB each) using a bitmap 
 * derived from Limine's boot memory map.
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"

#define PAGE_SIZE 4096

/**
 * @brief Round `value` up to the next multiple of PAGE_SIZE (unchanged if it
 *        is one already).
 */
static inline uint64_t page_align_up(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/**
 * @brief Initialize the Physical Memory Manager.
 * @param memmap Pointer to Limine memory map response.
 * @param hhdm_offset Virtual address offset of Higher-Half Direct Map.
 */
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/**
 * @brief Allocate a single 4KiB physical frame.
 * @return Physical base address of allocated frame, or NULL if OOM.
 */
void *pmm_alloc_frame(void);

/**
 * @brief Allocate multiple contiguous 4KiB physical frames.
 * @param count Number of contiguous frames to allocate.
 * @return Physical base address of first frame, or NULL if OOM.
 */
void *pmm_alloc_frames(size_t count);

/**
 * @brief Free a previously allocated physical frame.
 * @param phys_addr Physical base address of frame.
 */
void pmm_free_frame(void *phys_addr);

/**
 * @brief Free multiple physical frames.
 * @param phys_addr Physical base address of first frame.
 * @param count Number of contiguous frames to free.
 */
void pmm_free_frames(void *phys_addr, size_t count);

// Memory statistics
size_t pmm_get_total_frames(void);

/**
 * @brief Number of allocation requests refused for lack of memory since boot.
 */
size_t pmm_get_failed_allocations(void);
// Frames free now
size_t pmm_get_free_frames(void);
// Frames allocated now
size_t pmm_get_used_frames(void);
// Where the direct map starts: add this to a physical address to reach it
uint64_t pmm_get_hhdm_offset(void);

#endif // PMM_H
