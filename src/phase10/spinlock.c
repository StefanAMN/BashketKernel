/**
 * @file spinlock.c
 * @brief Implementation of Atomic Spinlock
 */

#include "spinlock.h"

void spinlock_acquire(spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void spinlock_release(spinlock_t *lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}
