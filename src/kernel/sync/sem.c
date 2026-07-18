/**
 * @file sem.c
 * @brief Implementation of Counting Semaphores
 */

#include "sem.h"
#include "sched.h"

void sem_init(sem_t *sem, int initial_value) {
    if (!sem) return;
    sem->value = initial_value;
    spinlock_init(&sem->lock);
}

void sem_wait(sem_t *sem) {
    if (!sem) return;
    while (1) {
        spinlock_acquire(&sem->lock);
        if (sem->value > 0) {
            sem->value--;
            spinlock_release(&sem->lock);
            break;
        }
        spinlock_release(&sem->lock);
        sched_yield();
    }
}

void sem_post(sem_t *sem) {
    if (!sem) return;
    spinlock_acquire(&sem->lock);
    sem->value++;
    spinlock_release(&sem->lock);
}

int sem_getvalue(sem_t *sem) {
    if (!sem) return 0;
    spinlock_acquire(&sem->lock);
    int val = sem->value;
    spinlock_release(&sem->lock);
    return val;
}
