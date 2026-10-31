/**
 * @file localsock.h
 * @brief Named listening sockets on this machine (track U2)
 *
 * A local connection is two pipes, one for each direction, and needs no code
 * here: file_open_local_pair makes one. This is the part that has a name. A
 * listener lives in a table under a number never used twice; the socket node
 * in the file tree holds only that number. So the node and the listener can
 * go separately, in either order: removing the name leaves the listener
 * running, unreachable, and closing the listener leaves a name whose connects
 * are refused, both as in POSIX.
 */

#ifndef LOCALSOCK_H
#define LOCALSOCK_H

#include <stdint.h>
#include <stddef.h>
#include "file.h"

#define LOCAL_BACKLOG_MAX 16   // Connections a listener can hold unaccepted
#define LOCAL_LISTENERS_MAX 32 // Listeners on the machine at once

typedef struct local_listener local_listener_t;

/**
 * @brief A new listener holding up to `backlog` connections (cut to
 *        1..LOCAL_BACKLOG_MAX).
 * @param id Set to its number, for a socket node's private_data.
 * @return The listener, or NULL if memory ran out or the table is full.
 */
local_listener_t *local_listener_create(int backlog, uint64_t *id);

/**
 * @brief No more connections: refuse those waiting for room, close those not
 *        accepted (their other ends read end-of-file), and free it. May sleep.
 *        Call once, when the last descriptor goes.
 */
void local_listener_close(local_listener_t *listener);

/**
 * @brief Connect to listener number `id`. Sleeps while its queue is full.
 * @param out Set to the caller's end, an open file, on success.
 * @return 0; -ECONNREFUSED if no such listener is open, or it closes while
 *         this waits; -EINTR on a signal; -ENOMEM. Holds nothing while it
 *         waits, so a kill there leaks nothing.
 */
int local_connect(uint64_t id, file_t **out);

/**
 * @brief The next connection to `listener`, waiting for one if there is none.
 * @param out Set to the accepting end, an open file, on success.
 * @return 0 or -EINTR.
 */
int local_accept(local_listener_t *listener, file_t **out);

/**
 * @brief For poll(): POLLIN when a connection waits to be accepted.
 */
int local_listener_poll(local_listener_t *listener);

#endif // LOCALSOCK_H
