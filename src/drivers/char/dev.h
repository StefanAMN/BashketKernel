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

#include "random.h" // get_random_u32 and get_random_bytes live there now

#endif // DEV_H
