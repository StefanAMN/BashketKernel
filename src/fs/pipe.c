/**
 * @file pipe.c
 * @brief Pipes: a one-way byte stream between processes
 *
 * A pipe is a ring buffer with a blocking reader and a blocking writer. Both
 * sides sleep on the pipe's address and wake each other after every transfer.
 * Each pipe has a lock, held around each check-then-sleep (see sched_sleep).
 */

#include "pipe.h"
#include "heap.h"
#include "sched.h"
#include "spinlock.h"
#include "kstring.h"
#include "kerrno.h"
#include "poll.h"

pipe_t *pipe_create(void) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(pipe_t));
    p->reader_open = true;
    p->writer_open = true;
    spinlock_init_ranked(&p->lock, "pipe", LOCK_RANK_PIPE);
    return p;
}

int64_t pipe_read(pipe_t *pipe, void *buf, size_t count) {
    if (!pipe || !buf) return -EINVAL;
    if (count == 0) return 0;

    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;

    uint64_t flags = spinlock_acquire_irqsave(&pipe->lock);
    while (pipe->count == 0 && pipe->writer_open) {
        if (sched_interrupted()) {
            spinlock_release_irqrestore(&pipe->lock, flags);
            return -EINTR;
        }
        sched_sleep(pipe, &pipe->lock, 0);
    }

    // Return what is there now, like a terminal: do not wait to fill `count`
    while (done < count && pipe->count > 0) {
        dst[done++] = pipe->buffer[pipe->tail];
        pipe->tail = (pipe->tail + 1) % PIPE_BUFFER_SIZE;
        pipe->count--;
    }

    if (done > 0) {
        sched_wakeup(pipe); // A writer may be waiting for room
        poll_notify();
    }
    spinlock_release_irqrestore(&pipe->lock, flags);
    return (int64_t)done;
}

int64_t pipe_write(pipe_t *pipe, const void *buf, size_t count) {
    if (!pipe || !buf) return -EINVAL;

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;

    uint64_t flags = spinlock_acquire_irqsave(&pipe->lock);
    while (done < count) {
        if (!pipe->reader_open) break; // Nobody will ever read this

        if (pipe->count == PIPE_BUFFER_SIZE) {
            if (sched_interrupted()) {
                // Report what did go through; EINTR only if nothing did
                spinlock_release_irqrestore(&pipe->lock, flags);
                return done > 0 ? (int64_t)done : -EINTR;
            }
            sched_sleep(pipe, &pipe->lock, 0);
            continue;
        }

        while (done < count && pipe->count < PIPE_BUFFER_SIZE) {
            pipe->buffer[pipe->head] = src[done++];
            pipe->head = (pipe->head + 1) % PIPE_BUFFER_SIZE;
            pipe->count++;
        }
        sched_wakeup(pipe); // A reader may be waiting for data
        poll_notify();
    }
    spinlock_release_irqrestore(&pipe->lock, flags);

    if (done == 0 && count > 0) return -EPIPE;
    return (int64_t)done;
}

// Mark one end (`end` points at reader_open or writer_open) closed, wake
// whoever waits at the other, and free the pipe if both ends are now closed
static void close_end(pipe_t *pipe, bool *end) {
    if (!pipe) return;

    uint64_t flags = spinlock_acquire_irqsave(&pipe->lock);
    *end = false;
    bool unused = !pipe->reader_open && !pipe->writer_open;
    if (!unused) sched_wakeup(pipe); // Let the other side notice
    spinlock_release_irqrestore(&pipe->lock, flags);

    if (unused) kfree(pipe);
    else poll_notify();
}

int pipe_poll(pipe_t *pipe, bool reader) {
    uint64_t flags = spinlock_acquire_irqsave(&pipe->lock);
    int events = 0;
    if (reader) {
        if (pipe->count > 0) events |= POLLIN;
        if (!pipe->writer_open) events |= POLLIN | POLLHUP;     // End of file: a read returns at once
    } else {
        if (!pipe->reader_open) events |= POLLERR;              // A write would fail (EPIPE)
        else if (pipe->count < PIPE_BUFFER_SIZE) events |= POLLOUT;
    }
    spinlock_release_irqrestore(&pipe->lock, flags);
    return events;
}

void pipe_close_reader(pipe_t *pipe) { if (pipe) close_end(pipe, &pipe->reader_open); }
void pipe_close_writer(pipe_t *pipe) { if (pipe) close_end(pipe, &pipe->writer_open); }
