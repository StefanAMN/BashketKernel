/**
 * @file sem.c
 * @brief Counting semaphores
 *
 * A task that finds the count at zero sleeps on the semaphore's address and
 * is woken by sem_post. Interrupts are disabled around the check-then-sleep
 * so a post from an interrupt handler cannot be missed.
 */

#include "sem.h"
#include "sched.h"

void sem_init(sem_t *sem, int initial_value) {
    if (!sem) return;
    sem->value = initial_value;
    spinlock_init_ranked(&sem->lock, "sem", LOCK_RANK_SEM);
}

void sem_wait(sem_t *sem) {
    if (!sem) return;

    uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
    while (sem->value <= 0) {
        sched_sleep(sem, &sem->lock, 0);
    }
    sem->value--;
    spinlock_release_irqrestore(&sem->lock, flags);
}

bool sem_trywait(sem_t *sem) {
    if (!sem) return false;

    uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
    bool taken = sem->value > 0;
    if (taken) sem->value--;
    spinlock_release_irqrestore(&sem->lock, flags);
    return taken;
}

void sem_post(sem_t *sem) {
    if (!sem) return;

    uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
    sem->value++;
    sched_wakeup(sem);
    spinlock_release_irqrestore(&sem->lock, flags);
}

int sem_getvalue(sem_t *sem) {
    if (!sem) return 0;
    return sem->value;
}
