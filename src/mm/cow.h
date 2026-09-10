/**
 * @file cow.h
 * @brief Physical frame sharing counts (the basis for copy-on-write)
 *
 * A frame handed out by the PMM has one owner. When a second address space
 * maps the same frame (fork), it takes a reference. Each owner drops its
 * reference when it unmaps the frame; the frame returns to the PMM only when
 * the last owner lets go.
 */

#ifndef COW_H
#define COW_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Allocate the sharing table, one counter per physical frame.
 */
void cow_init(void);

/**
 * @brief Record one more owner of a frame.
 */
void pmm_ref_frame(void *phys_frame);

/**
 * @brief Drop one owner. Frees the frame if that was the last one.
 */
void pmm_unref_frame(void *phys_frame);

/**
 * @brief Number of owners of an allocated frame (1 = not shared).
 */
uint32_t pmm_get_frame_ref(void *phys_frame);

#endif // COW_H
