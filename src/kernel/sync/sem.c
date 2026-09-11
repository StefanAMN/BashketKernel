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
    spinlock_init(&sem->lock);
}

void sem_wait(sem_t *sem) {
    if (!sem) return;

    uint64_t flags = irq_save();
    while (sem->value <= 0) {
        sched_block(sem, 0);
    }
    sem->value--;
    irq_restore(flags);
}

bool sem_trywait(sem_t *sem) {
    if (!sem) return false;

    uint64_t flags = irq_save();
    bool taken = sem->value > 0;
    if (taken) sem->value--;
    irq_restore(flags);
    return taken;
}

void sem_post(sem_t *sem) {
    if (!sem) return;

    uint64_t flags = irq_save();
    sem->value++;
    sched_wakeup(sem);
    irq_restore(flags);
}

int sem_getvalue(sem_t *sem) {
    if (!sem) return 0;
    return sem->value;
}
