/**
 * @file tcp.h
 * @brief TCP: reliable byte streams over the IPv4 layer in net.c
 *
 * What is implemented (RFC 9293): opening a connection from either side
 * (three-way handshake), sequence and acknowledgement numbers, retransmission
 * on a timer with exponential back-off, flow control by the receiver's
 * advertised window (with a probe when it is zero), orderly close from
 * either side, and reset. Also:
 *  - Reassembly: a segment that arrives ahead of the one expected is kept
 *    (in the free part of the receive ring, at the place its sequence number
 *    gives it, so it costs no memory of its own) and delivered when the gap
 *    before it fills. Up to TCP_OOO_RANGES separate stretches are kept;
 *    one that would need more is dropped, as before.
 *  - A retransmission time measured from round trips (RFC 6298): one
 *    segment at a time is timed, never one that was sent twice (Karn's
 *    rule), and the timeout is SRTT + 4 * RTTVAR.
 *  - Fast retransmit (RFC 5681, 3.2): three duplicate acknowledgements
 *    send the oldest unacknowledged segment again at once, without waiting
 *    for the timer. The receiver acknowledges every out-of-order segment at
 *    once, so that a sender (ours or another) gets its duplicates.
 *
 * What is not, and what follows from it:
 *  - No congestion control (RFC 5681's congestion window, slow start, fast
 *    recovery). The sender is limited only by the receiver's window.
 *    Harmless on QEMU's virtual network; not fit for a real one. When the
 *    timer runs out, everything from the oldest unacknowledged byte is
 *    sent again ("go-back-N"), which with no congestion window means a
 *    whole window at once.
 *  - The RFC 6298 limits are changed for a virtual network: the first
 *    timeout is 200 ms rather than 1 s (2.1), the floor is 200 ms rather
 *    than 1 s (2.4), and the timeout (backed off or measured) is capped
 *    at 3 s rather than at least 60 s (2.5), so a dead peer is given up
 *    after about 20 s.
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
    uint64_t out_of_order_kept;  // ... ahead, and kept until the gap filled
    uint64_t fast_retransmissions; // Sent again after three duplicate acknowledgements
    uint64_t rtt_samples;        // Round trips measured
    uint64_t dropped_for_test;   // See tcp_test_drop_every
} tcp_stats_t;

// --- Pieces with no state of their own, public for the tests ---

#define TCP_OOO_RANGES 8         // Separate stretches of data kept ahead of the gap

// Received data beyond the next byte expected: sorted stretches of sequence
// numbers [start, end), never touching or overlapping one another
typedef struct {
    struct { uint32_t start, end; } range[TCP_OOO_RANGES];
    uint32_t count;
} tcp_ooo_t;

/**
 * @brief Note that [start, end) has arrived (start before end, both ahead of
 *        what is expected). Stretches it touches or overlaps are joined.
 * @return false (and nothing changed) if it would need more than
 *         TCP_OOO_RANGES stretches.
 */
bool tcp_ooo_add(tcp_ooo_t *q, uint32_t start, uint32_t end);

/**
 * @brief Everything up to `rcv_nxt` is in: forget the stretches it reaches,
 * @return the next byte now expected (past any stretch that joins on).
 */
uint32_t tcp_ooo_take(tcp_ooo_t *q, uint32_t rcv_nxt);

// The round-trip estimate of RFC 6298, in microseconds
typedef struct {
    uint32_t srtt_us, rttvar_us;
    uint32_t rto_us;             // Clamped to the limits in tcp.c
    bool measured;               // Whether there has been a sample yet
} tcp_rtt_t;

#define TCP_RTO_MIN_US 200000u   // Floor of the timeout (RFC 6298 2.4 says 1 s: see above)
#define TCP_RTO_MAX_US 3000000u  // Cap (2.5 says at least 60 s: see above)
#define TCP_CLOCK_GRANULARITY_US 10000u // G in RFC 6298: the timers run in 10 ms ticks

/**
 * @brief Fold one round-trip measurement `r_us` into the estimate (RFC 6298
 *        2.2 for the first, 2.3 for the rest) and compute the timeout.
 */
void tcp_rtt_sample(tcp_rtt_t *t, uint32_t r_us);

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
 * @brief For poll(). A listening socket: POLLIN when a connection waits for
 *        tcp_accept. A connection: POLLIN when there is data or the other
 *        end has finished (or the connection failed: POLLERR and POLLHUP
 *        too), POLLOUT while there is room to write, POLLHUP once closed.
 */
int tcp_poll(tcp_socket_t *sock);

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

/**
 * @brief For tests: of the connection between local_port and remote_port
 *        (at our own address), the next sequence number it expects, and
 *        the acknowledgement number its peer should send now (all it has
 *        sent so far, so its SYN only once that has gone out). false if
 *        there is no such connection.
 */
bool tcp_test_sequences(uint16_t local_port, uint16_t remote_port, uint32_t *rcv_nxt, uint32_t *ack);

#endif // TCP_H
