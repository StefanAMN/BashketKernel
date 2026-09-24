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
 *  - Congestion control (RFC 5681): the sender keeps no more than the
 *    smaller of the receiver's window and its own congestion window in
 *    flight. That window starts at three or four segments (3.1, eq. 1),
 *    grows by a segment per acknowledgement in slow start (eq. 2) and by
 *    about one per round trip above ssthresh (eq. 3). A timeout sets it
 *    to one segment and ssthresh to half of what was in flight (eq. 4);
 *    fast retransmit sets ssthresh the same way and starts fast recovery,
 *    in NewReno's form (RFC 6582): a partial acknowledgement sends the
 *    next hole at once and recovery ends only when everything sent before
 *    it began is acknowledged. /proc/tcp shows each connection's window.
 *
 * What is not, and what follows from it:
 *  - No limited transmit (RFC 3042) and no selective acknowledgement: in
 *    a window with several losses, NewReno repairs one hole per round
 *    trip. After a timeout, everything from the oldest unacknowledged
 *    byte is sent again ("go-back-N"), but at the pace slow start allows.
 *  - No congestion window validation (RFC 7661): a connection limited by
 *    the receiver's window keeps growing its congestion window, up to
 *    TCP_CWND_MAX.
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
    uint64_t recoveries;         // Fast recoveries begun (RFC 5681 3.2)
    uint64_t partial_acks;       // Partial acknowledgements in recovery (RFC 6582)
    uint64_t cwnd_timeouts;      // Timer expiries that set a congestion window to one segment
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

// Congestion control (RFC 5681), with NewReno's fast recovery (RFC 6582).
// Sizes in bytes; smss is the sender's maximum segment size.
typedef struct {
    uint32_t cwnd;               // Congestion window
    uint32_t ssthresh;           // Slow start below it, congestion avoidance at or above
    bool in_recovery;            // Between a fast retransmit and the full acknowledgement
    uint32_t recover;            // RFC 6582: highest sequence number sent when recovery (or a timeout) began
} tcp_cc_t;

#define TCP_CWND_MAX (4u * 65535u) // Grown no further: no window beyond 64 KiB can be used (no scaling)

/**
 * @brief A connection is established: the initial window (RFC 5681 3.1,
 *        eq. 1), an "arbitrarily high" ssthresh (the largest window the
 *        other end can advertise), recover = iss (RFC 6582 3.2).
 */
void tcp_cc_init(tcp_cc_t *cc, uint32_t smss, uint32_t iss);

/**
 * @brief New data acknowledged: `acked` bytes, so that the acknowledgement
 *        is now `ack` and `flight` bytes are still outstanding. Grows the
 *        window (slow start or congestion avoidance), or in recovery deals
 *        with a partial or full acknowledgement (RFC 6582 3.2, 5 and 6).
 * @return true for a partial acknowledgement: the segment it points at must
 *         be sent again at once.
 */
bool tcp_cc_new_ack(tcp_cc_t *cc, uint32_t smss, uint32_t acked, uint32_t ack, uint32_t flight);

/**
 * @brief A duplicate acknowledgement of `ack`, the `count`th in a row, with
 *        snd_max one past the highest byte sent and `flight` outstanding.
 *        In recovery each one inflates the window by a segment (RFC 5681
 *        3.2, 4); the third outside it starts recovery, unless `ack` does
 *        not go beyond `recover` (RFC 6582 3.2, 1 and 2).
 * @return true when the segment at `ack` must be sent again now (fast
 *         retransmit).
 */
bool tcp_cc_dup_ack(tcp_cc_t *cc, uint32_t smss, uint32_t count, uint32_t ack, uint32_t snd_max, uint32_t flight);

/**
 * @brief The retransmission timer expired with `flight` outstanding and
 *        snd_max one past the highest byte sent: the window becomes one
 *        segment (RFC 5681 3.1), ssthresh half the flight if `first`
 *        (only the first expiry for a segment changes it, eq. 4), and any
 *        recovery ends, with recover moved to snd_max - 1 (RFC 6582 3.2, 4).
 */
void tcp_cc_timeout(tcp_cc_t *cc, uint32_t smss, uint32_t flight, uint32_t snd_max, bool first);

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
 * @brief Describe every connection as text into buf (at most size bytes,
 *        NUL-terminated): ports, peer, state, congestion window, ssthresh,
 *        smoothed round trip and timeout. For /proc/tcp.
 * @return the length written.
 */
size_t tcp_describe(char *buf, size_t size);

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
 *        sent so far, so its SYN only once that has gone out), and the
 *        oldest of its sequence numbers not yet acknowledged (its initial
 *        sequence number until the handshake ends, sent or not). false if
 *        there is no such connection.
 */
bool tcp_test_sequences(uint16_t local_port, uint16_t remote_port, uint32_t *rcv_nxt, uint32_t *ack,
                        uint32_t *unacked);

#define TCP_TRACE_MAX 1024

/**
 * @brief For tests: record the congestion window of connections to remote
 *        port `port` each time it changes (TCP_TRACE_MAX values at most,
 *        after which recording stops); 0 stops, and starting clears.
 */
void tcp_test_trace_cwnd(uint16_t port);

/**
 * @brief For tests: copy up to `max` recorded values into out.
 * @return how many there are.
 */
size_t tcp_test_cwnd_trace(uint32_t *out, size_t max);

#endif // TCP_H
