/**
 * @file localsock.c
 * @brief Named listening sockets on this machine (see localsock.h)
 *
 * A task killed in its sleep ends there, without unwinding (sched_sleep), so
 * nothing may be held across a wait: a connect waiting for room owns no files
 * and no reference, and looks the listener up again by number when it wakes.
 * That is why one lock covers the table and every queue, and why the channel
 * slept on is the number rather than the listener, which may be gone.
 */

#include "localsock.h"
#include "heap.h"
#include "kerrno.h"
#include "kstring.h"
#include "poll.h"
#include "sched.h"
#include "spinlock.h"

struct local_listener {
    file_t *queue[LOCAL_BACKLOG_MAX];  // Connections not accepted yet, oldest at `head`
    size_t head, count;
    size_t capacity;
    uint64_t id;
};

static spinlock_t table_lock = SPINLOCK_RANKED("local-listeners", LOCK_RANK_PIPE);
static local_listener_t *table[LOCAL_LISTENERS_MAX];
static uint64_t next_id = 1;           // Never reused; 0 is no listener

// What tasks waiting on listener `id` sleep on. Small numbers are no kernel
// address, and a wake-up that reaches a wrong sleeper only makes it look again.
static void *channel_of(uint64_t id) {
    return (void *)id;
}

// The open listener with this number, or NULL. Caller holds table_lock.
static local_listener_t *find_locked(uint64_t id) {
    for (size_t i = 0; i < LOCAL_LISTENERS_MAX; i++) {
        if (table[i] && table[i]->id == id) return table[i];
    }
    return NULL;
}

local_listener_t *local_listener_create(int backlog, uint64_t *id) {
    local_listener_t *listener = kmalloc(sizeof(local_listener_t));
    if (!listener) return NULL;
    memset(listener, 0, sizeof(*listener));
    listener->capacity = backlog < 1 ? 1 : backlog > LOCAL_BACKLOG_MAX ? LOCAL_BACKLOG_MAX : (size_t)backlog;

    uint64_t flags = spinlock_acquire_irqsave(&table_lock);
    for (size_t i = 0; i < LOCAL_LISTENERS_MAX; i++) {
        if (!table[i]) {
            table[i] = listener;
            listener->id = next_id++;
            *id = listener->id;
            spinlock_release_irqrestore(&table_lock, flags);
            return listener;
        }
    }
    spinlock_release_irqrestore(&table_lock, flags);
    kfree(listener);
    return NULL;
}

void local_listener_close(local_listener_t *listener) {
    file_t *waiting[LOCAL_BACKLOG_MAX];
    uint64_t flags = spinlock_acquire_irqsave(&table_lock);
    for (size_t i = 0; i < LOCAL_LISTENERS_MAX; i++) {
        if (table[i] == listener) table[i] = NULL;
    }
    size_t count = listener->count;
    for (size_t i = 0; i < count; i++) waiting[i] = listener->queue[(listener->head + i) % LOCAL_BACKLOG_MAX];
    sched_wakeup(channel_of(listener->id));   // Connects waiting for room find it gone
    spinlock_release_irqrestore(&table_lock, flags);

    for (size_t i = 0; i < count; i++) file_close(waiting[i]);   // May sleep: outside the lock
    kfree(listener);
}

int local_connect(uint64_t id, file_t **out) {
    file_t *mine = NULL, *theirs = NULL;
    uint64_t flags = spinlock_acquire_irqsave(&table_lock);
    for (;;) {
        local_listener_t *listener = find_locked(id);
        if (!listener) {
            spinlock_release_irqrestore(&table_lock, flags);
            if (mine) {
                file_close(mine);
                file_close(theirs);
            }
            return -ECONNREFUSED;
        }
        if (listener->count < listener->capacity) {
            if (!mine) {
                // Room, and nothing to put there yet. Made outside the lock;
                // the room may be gone when it is looked at again.
                spinlock_release_irqrestore(&table_lock, flags);
                int err = file_open_local_pair(&mine, &theirs);
                if (err != 0) return err;
                flags = spinlock_acquire_irqsave(&table_lock);
                continue;
            }
            listener->queue[(listener->head + listener->count) % LOCAL_BACKLOG_MAX] = theirs;
            listener->count++;
            sched_wakeup(channel_of(id));     // An accept may be waiting
            poll_notify();
            spinlock_release_irqrestore(&table_lock, flags);
            *out = mine;
            return 0;
        }
        if (mine) {
            // Full after all. Nothing may be held across the wait (see the top).
            spinlock_release_irqrestore(&table_lock, flags);
            file_close(mine);
            file_close(theirs);
            mine = theirs = NULL;
            flags = spinlock_acquire_irqsave(&table_lock);
            continue;
        }
        if (sched_interrupted()) {
            spinlock_release_irqrestore(&table_lock, flags);
            return -EINTR;
        }
        sched_sleep(channel_of(id), &table_lock, 0);
    }
}

int local_accept(local_listener_t *listener, file_t **out) {
    uint64_t id = listener->id;
    uint64_t flags = spinlock_acquire_irqsave(&table_lock);
    while (listener->count == 0) {
        if (sched_interrupted()) {
            spinlock_release_irqrestore(&table_lock, flags);
            return -EINTR;
        }
        sched_sleep(channel_of(id), &table_lock, 0);
    }
    *out = listener->queue[listener->head];
    listener->head = (listener->head + 1) % LOCAL_BACKLOG_MAX;
    listener->count--;
    sched_wakeup(channel_of(id));             // A connect may be waiting for room
    spinlock_release_irqrestore(&table_lock, flags);
    return 0;
}

int local_listener_poll(local_listener_t *listener) {
    uint64_t flags = spinlock_acquire_irqsave(&table_lock);
    int ready = listener->count > 0 ? POLLIN : 0;
    spinlock_release_irqrestore(&table_lock, flags);
    return ready;
}
