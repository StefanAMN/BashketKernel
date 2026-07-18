/**
 * @file spinlock.h
 * @brief Multiprocessor Spinlock Primitives
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT ((spinlock_t){0})

/**
 * @brief Initialize spinlock structure.
 */
static inline void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

/**
 * @brief Acquire spinlock (busy-waits with pause instruction until lock is free).
 */
void spinlock_acquire(spinlock_t *lock);

/**
 * @brief Release spinlock atomically.
 */
void spinlock_release(spinlock_t *lock);

#endif // SPINLOCK_H
