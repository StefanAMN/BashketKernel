/**
 * @file dev.h
 * @brief Device nodes under /dev
 */

#ifndef DEV_H
#define DEV_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Create /dev/null, /dev/zero, /dev/random and /dev/console.
 */
void dev_init(void);

/**
 * @brief Pseudo-random numbers (xorshift64*, seeded from the CPU timestamp
 *        counter). Good for tests and games. NOT suitable for cryptography.
 */
uint32_t get_random_u32(void);
void get_random_bytes(void *buf, size_t count);

#endif // DEV_H
