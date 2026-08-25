/**
 * @file hrtimer.h
 * @brief High-Resolution Timers & Nanosleep Interface
 */

#ifndef HRTIMER_H
#define HRTIMER_H

#include <stdint.h>

/**
 * @brief Initialize High-Resolution Timer subsystem.
 */
void hrtimer_init(void);

/**
 * @brief Pause execution for specified duration in nanoseconds.
 * @param nanoseconds Duration in nanoseconds.
 */
void nanosleep(uint64_t nanoseconds);

#endif // HRTIMER_H
