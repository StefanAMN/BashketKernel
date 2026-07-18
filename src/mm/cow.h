/**
 * @file cow.h
 * @brief Copy-On-Write (COW) Frame Reference Counting Interface
 */

#ifndef COW_H
#define COW_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize Frame Reference Counter array based on total PMM physical frames.
 */
void cow_init(void);

/**
 * @brief Increment reference counter for physical frame.
 * @param phys_frame Physical frame address.
 */
void pmm_ref_frame(void *phys_frame);

/**
 * @brief Decrement reference counter for physical frame. If refcount drops to 0, frame is freed.
 * @param phys_frame Physical frame address.
 */
void pmm_unref_frame(void *phys_frame);

/**
 * @brief Get current reference count of physical frame.
 * @param phys_frame Physical frame address.
 * @return Number of active references.
 */
uint16_t pmm_get_frame_ref(void *phys_frame);

#endif // COW_H
