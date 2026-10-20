/**
 * @file poll.h
 * @brief Waiting for any of several open files to be ready
 *
 * A program that talks to two things at once (the terminal and a network
 * connection, the mouse and the keyboard) cannot block in read() on one of
 * them. poll() takes a list of descriptors with what it wants of each
 * (to read, to write) and sleeps until at least one can be done without
 * waiting, or a time limit passes.
 *
 * Each kind of open file says whether it is ready (file_poll). Waking a
 * poller does not need a list of who polls what: anything whose readiness
 * may have changed calls poll_notify(), which wakes every poller, and each
 * looks at its own files again. A poller notes a counter of notifications
 * before it looks and sleeps only if no notification came since, so one
 * that arrives while it is looking is not lost.
 */

#ifndef POLL_H
#define POLL_H

#include <stdint.h>
#include <stddef.h>
#include "file.h"

// Events (the values are Linux's)
#define POLLIN   0x001   // Something to read, or the end of the stream
#define POLLOUT  0x004   // A write would not wait
#define POLLERR  0x008   // An error (a pipe whose reader is gone, a reset connection)
#define POLLHUP  0x010   // The other end is gone (reported whether asked for or not)
#define POLLNVAL 0x020   // Not an open descriptor

#define POLL_MAX 64      // Most descriptors one call may ask about

// One entry of the array poll() works on, as programs pass it
struct pollfd {
    int32_t fd;
    int16_t events;      // What the caller wants to know about
    int16_t revents;     // Filled in: what is the case (of events, plus ERR, HUP, NVAL)
};

/**
 * @brief Which events are true of `file` right now (POLLIN, POLLOUT,
 *        POLLERR, POLLHUP), whatever was asked. May take locks; never
 *        sleeps. For the terminal in line mode, keys already typed are
 *        put through the line discipline first, so a complete line counts.
 */
int file_poll(file_t *file);

/**
 * @brief Tell pollers that something may have become ready. Cheap; safe
 *        from interrupt handlers and with any lock of rank below
 *        LOCK_RANK_POLL held.
 */
void poll_notify(void);

/**
 * @brief Fill in revents for each entry of `fds` (kernel memory) against
 *        the descriptor table `table`, sleeping until at least one has an
 *        event or `timeout_ms` passes (negative: no limit; 0: just look).
 * @return The number of entries with an event, 0 on timeout, or -EINTR.
 */
int64_t poll_wait(file_t *const table[], struct pollfd *fds, size_t count, int64_t timeout_ms);

#endif // POLL_H
