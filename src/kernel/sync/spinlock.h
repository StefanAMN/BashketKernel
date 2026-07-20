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

/**
 * @brief Disable interrupts, then acquire the lock.
 *
 * Use this for any data that an interrupt handler (or a task switched in by
 * the timer interrupt) can also touch. Taking a plain spinlock there would
 * deadlock: the interrupted code holds the lock and can never run to release it.
 *
 * @return The RFLAGS value from before interrupts were disabled.
 */
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);

/**
 * @brief Release the lock, then restore the interrupt flag saved by
 * spinlock_acquire_irqsave. Interrupts are only re-enabled if they were
 * enabled before, so nested critical sections work.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags);

/**
 * @brief Save RFLAGS and disable interrupts, without taking a lock.
 */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

/**
 * @brief Re-enable interrupts if they were enabled in the saved RFLAGS.
 */
static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200) { // RFLAGS.IF
        asm volatile("sti");
    }
}

#endif // SPINLOCK_H
