/**
 * @file tcp.h
 * @brief TCP: reliable byte streams over the IPv4 layer in net.c
 *
 * What is implemented (RFC 9293): opening a connection from either side
 * (three-way handshake), sequence and acknowledgement numbers, retransmission
 * on a timer with exponential back-off, flow control by the receiver's
 * advertised window (with a probe when it is zero), orderly close from
 * either side, and reset.
 *
 * What is not, and what follows from it:
 *  - No congestion control. The sender is limited only by the receiver's
 *    window. Harmless on QEMU's virtual network; not fit for a real one.
 *  - No reassembly: a segment that arrives ahead of the one expected is
 *    dropped and acknowledged again, and the sender's timer brings it back.
 *    After a loss, everything from the lost segment on is sent again
 *    ("go-back-N").
 *  - The retransmission time starts at a fixed value rather than being
 *    measured from round trips, and there is no fast retransmit.
 *  - No options except the maximum segment size: no window scaling (so at
 *    most 64 KiB in flight), no timestamps, no selective acknowledgement.
 *  - No simultaneous open, no urgent data, no keep-alive.
 */

#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TCP_MAX_SOCKETS 32
#define TCP_BUFFER_SIZE 8192   // Per direction, per connection
#define TCP_BACKLOG_MAX 8

typedef struct tcp_socket tcp_socket_t;

typedef struct {
    uint64_t segments_in, segments_out;
    uint64_t retransmissions;    // Timer expiries that sent something again
    uint64_t bad_segments;       // Short, or wrong checksum
    uint64_t resets_sent, resets_received;
    uint64_t out_of_order;       // Arrived ahead of or behind what was expected
    uint64_t dropped_for_test;   // See tcp_test_drop_every
} tcp_stats_t;

/**
 * @brief Start the TCP timer thread. Called by net_init.
 */
void tcp_init(void);

/**
 * @brief A segment from the IP layer.
 */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *segment, size_t len);

/**
 * @brief A new, unconnected socket (NULL if there are none left).
 */
tcp_socket_t *tcp_open(void);

/**
 * @brief Give the socket a local port (before tcp_listen).
 */
int tcp_bind(tcp_socket_t *sock, uint16_t port);

/**
 * @brief Connect to ip:port. Sleeps until the connection is made or fails.
 * @return 0, or -ECONNREFUSED, -ETIMEDOUT, -EHOSTUNREACH, -EINTR...
 */
int tcp_connect(tcp_socket_t *sock, uint32_t ip, uint16_t port);

/**
 * @brief Accept connections to the bound port. Up to `backlog` may wait to
 *        be taken with tcp_accept.
 */
int tcp_listen(tcp_socket_t *sock, int backlog);

/**
 * @brief Take the next completed connection, sleeping until there is one.
 * @return The connection, or NULL with *error set (negative errno).
 */
tcp_socket_t *tcp_accept(tcp_socket_t *sock, int *error);

/**
 * @brief Read what has arrived, sleeping while nothing has.
 * @return Bytes read; 0 once the other end has closed and all is read.
 */
int64_t tcp_read(tcp_socket_t *sock, void *buf, size_t len);

/**
 * @brief Queue data to be sent, sleeping while the send buffer is full.
 * @return `len`, or fewer if interrupted part-way, or a negative errno.
 */
int64_t tcp_write(tcp_socket_t *sock, const void *buf, size_t len);

/**
 * @brief The owner is done with the socket. What is queued is still sent
 *        and the connection is closed in the background.
 */
void tcp_close(tcp_socket_t *sock);

/**
 * @brief Finish sending: the other end reads end-of-stream once it has
 *        everything written so far. Reading goes on as before.
 */
int tcp_shutdown(tcp_socket_t *sock);

/**
 * @brief The other end's address (0 and 0 if not connected).
 */
void tcp_peer(tcp_socket_t *sock, uint32_t *ip, uint16_t *port);

/**
 * @brief TCP's counters since boot (read without a lock: for tests).
 */
const tcp_stats_t *tcp_stats(void);

/**
 * @brief Number of sockets in use, including connections still closing in
 *        the background.
 */
size_t tcp_sockets_in_use(void);

/**
 * @brief For tests: throw away one segment in n instead of sending it
 *        (0: none; 1: all), chosen by a fixed pseudo-random sequence, to
 *        exercise retransmission.
 */
void tcp_test_drop_every(uint32_t n);

#endif // TCP_H
