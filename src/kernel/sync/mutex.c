/**
 * @file mutex.c
 * @brief A lock that may be held across a sleep (see mutex.h)
 */

#include "mutex.h"
#include "sched.h"

// Caller holds mutex->lock. Before the scheduler starts there is no current
// task: the one thread of execution there is counts as the owner.
static bool take_if_possible(mutex_t *mutex, struct task *self) {
    if (mutex->taken && mutex->owner != self) return false;
    mutex->taken = true;
    mutex->owner = self;
    mutex->depth++;
    return true;
}

void mutex_lock(mutex_t *mutex) {
    struct task *self = sched_get_current_task();
    uint64_t flags = spinlock_acquire_irqsave(&mutex->lock);
    while (!take_if_possible(mutex, self)) {
        sched_sleep_uninterruptible(mutex, &mutex->lock, 0);
    }
    spinlock_release_irqrestore(&mutex->lock, flags);
}

bool mutex_trylock(mutex_t *mutex) {
    uint64_t flags = spinlock_acquire_irqsave(&mutex->lock);
    bool got = take_if_possible(mutex, sched_get_current_task());
    spinlock_release_irqrestore(&mutex->lock, flags);
    return got;
}

void mutex_unlock(mutex_t *mutex) {
    uint64_t flags = spinlock_acquire_irqsave(&mutex->lock);
    bool released = false;
    if (mutex->depth > 0 && --mutex->depth == 0) {
        mutex->taken = false;
        mutex->owner = NULL;
        released = true;
    }
    spinlock_release_irqrestore(&mutex->lock, flags);
    if (released) sched_wakeup(mutex);
}

bool mutex_held(mutex_t *mutex) {
    return mutex->taken && mutex->owner == sched_get_current_task();
}
