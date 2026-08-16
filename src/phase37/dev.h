/**
 * @file dev.h
 * @brief Virtual Hardware Device Manager (/dev) Interface
 */

#ifndef DEV_H
#define DEV_H

#include <stdint.h>

/**
 * @brief Initialize Virtual Device Manager and mount '/dev' nodes.
 */
void dev_init(void);

/**
 * @brief Generate 32-bit pseudo-random integer using CPU RDTSC hardware timer.
 * @return 32-bit random integer value.
 */
uint32_t get_random_u32(void);

#endif // DEV_H
