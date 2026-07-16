/**
 * @file swap.h
 * @brief Virtual Memory Swap Manager & Demand Paging Interface
 */

#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize swap page manager.
 * @param max_swap_frames Total capacity in 4KiB frames.
 */
void swap_init(size_t max_swap_frames);

/**
 * @brief Allocate a swap slot and swap out a physical memory frame.
 * @param phys_addr Physical address of frame to swap out.
 * @return Allocated swap slot ID, or 0xFFFFFFFF on failure.
 */
uint32_t swap_out_frame(uint64_t phys_addr);

/**
 * @brief Swap in frame from swap slot back into physical RAM.
 * @param swap_slot Target swap slot ID to restore.
 * @return Restored physical frame address, or 0 on failure.
 */
uint64_t swap_in_frame(uint32_t swap_slot);

/**
 * @brief Get total available swap slot count.
 * @return Number of free swap slots.
 */
size_t swap_get_free_slots(void);

#endif // SWAP_H
