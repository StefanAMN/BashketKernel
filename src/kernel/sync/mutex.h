/**
 * @file mutex.h
 * @brief A lock that may be held across a sleep
 *
 * A spinlock holder must not sleep. Some things need to stay locked while
 * their holder waits, for the disk above all: a file system change is many
 * disk requests long. A mutex does that. A task that finds it taken sleeps
 * until it is free, so a mutex can only be used from task context, never
 * from an interrupt handler.
 */

#ifndef MUTEX_H
#define MUTEX_H

#include "spinlock.h"

struct task;

typedef struct {
    spinlock_t lock;        // Protects the two fields below
    bool taken;
    struct task *owner;     // Who holds it
    uint32_t depth;         // How many times the owner has taken it
} mutex_t;

#define MUTEX_INIT(name) { SPINLOCK_RANKED(name, LOCK_RANK_MUTEX), false, 0, 0 }

/**
 * @brief Take the mutex, sleeping until it is free. The sleep is not cut
 *        short by a kill or a signal: callers go on to change what it
 *        protects and could not back out.
 *
 * The task that holds a mutex may take it again (and must then unlock it as
 * many times). The file system layer relies on this: its public functions
 * each take the lock and also call one another.
 */
void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);

/**
 * @brief Take the mutex if it is free; never sleeps.
 */
bool mutex_trylock(mutex_t *mutex);

/**
 * @brief Whether the calling task holds it.
 */
bool mutex_held(mutex_t *mutex);

#endif // MUTEX_H
