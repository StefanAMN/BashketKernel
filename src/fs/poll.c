/**
 * @file poll.c
 * @brief Waiting for any of several open files to be ready (see poll.h)
 */

#include "poll.h"
#include "localsock.h"
#include "pipe.h"
#include "tty.h"
#include "net.h"
#include "tcp.h"
#include "sched.h"
#include "spinlock.h"
#include "pit.h"
#include "kerrno.h"

// Notifications so far, and the channel pollers sleep on (its address)
static uint64_t generation = 0;
static spinlock_t poll_lock = SPINLOCK_RANKED("poll", LOCK_RANK_POLL);

void poll_notify(void) {
    uint64_t flags = spinlock_acquire_irqsave(&poll_lock);
    generation++;
    sched_wakeup(&generation);
    spinlock_release_irqrestore(&poll_lock, flags);
}

int file_poll(file_t *file) {
    switch (file->kind) {
        case FILE_CONSOLE: return tty_poll();
        case FILE_PIPE:    return pipe_poll(file->pipe, file->readable);
        case FILE_SOCKET:  return udp_poll(file->socket);
        case FILE_TCP:     return tcp_poll(file->tcp);
        case FILE_LOCAL:
            return pipe_poll(file->local_in, true) |
                   (__atomic_load_n(&file->local_shut, __ATOMIC_ACQUIRE) ? 0 : pipe_poll(file->local_out, false));
        case FILE_LOCAL_LISTENER: return local_listener_poll(file->listener);
        case FILE_VNODE:
            // A device says for itself; anything else (a file, a directory)
            // never makes a reader or a writer wait
            if (file->node->ops && file->node->ops->poll) return file->node->ops->poll(file->node);
            return POLLIN | POLLOUT;
    }
    return POLLERR;
}

// Look at every entry once: fill in revents, return how many have one
static int64_t scan(file_t *const table[], struct pollfd *fds, size_t count) {
    int64_t ready = 0;
    for (size_t i = 0; i < count; i++) {
        int fd = fds[i].fd;
        if (fd < 0) {                                    // Ignored, as POSIX says
            fds[i].revents = 0;
            continue;
        }
        file_t *file = fd < TASK_MAX_FDS ? table[fd] : NULL;
        int events = file ? file_poll(file) : POLLNVAL;
        // What was asked for, and what is always reported
        fds[i].revents = (int16_t)(events & (fds[i].events | POLLERR | POLLHUP | POLLNVAL));
        if (fds[i].revents) ready++;
    }
    return ready;
}

int64_t poll_wait(file_t *const table[], struct pollfd *fds, size_t count, int64_t timeout_ms) {
    // The limit in timer ticks, rounded up (0: none)
    uint64_t deadline = 0;
    if (timeout_ms > 0) deadline = timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000;

    for (;;) {
        uint64_t flags = spinlock_acquire_irqsave(&poll_lock);
        uint64_t seen = generation;
        spinlock_release_irqrestore(&poll_lock, flags);

        int64_t ready = scan(table, fds, count);
        if (ready > 0 || timeout_ms == 0) return ready;
        if (sched_interrupted()) return -EINTR;
        if (deadline && timer_ticks() >= deadline) return 0;

        // Asleep only if nothing was notified while we looked
        flags = spinlock_acquire_irqsave(&poll_lock);
        if (generation == seen) sched_sleep(&generation, &poll_lock, deadline);
        spinlock_release_irqrestore(&poll_lock, flags);
    }
}
