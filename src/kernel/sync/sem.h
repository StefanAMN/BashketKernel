/**
 * @file sem.h
 * @brief Counting Semaphores & Thread Synchronization Interface
 */

#ifndef SEM_H
#define SEM_H

#include <stdint.h>
#include "spinlock.h"

typedef struct {
    volatile int value;
    spinlock_t lock;
} sem_t;

/**
 * @brief Initialize a counting semaphore.
 * @param sem Pointer to semaphore structure.
 * @param initial_value Initial integer value count.
 */
void sem_init(sem_t *sem, int initial_value);

/**
 * @brief Decrement (wait / P) semaphore value.
 * @param sem Target semaphore.
 */
void sem_wait(sem_t *sem);

/**
 * @brief Increment (post / signal / V) semaphore value.
 * @param sem Target semaphore.
 */
void sem_post(sem_t *sem);

/**
 * @brief Retrieve current value count of semaphore.
 * @param sem Target semaphore.
 * @return Current integer value.
 */
int sem_getvalue(sem_t *sem);

#endif // SEM_H
