#include "test.h"
#include "tcp.h"
#include "net.h"
#include "pit.h"
#include "hrtimer.h"
#include "heap.h"
#include "sched.h"
#include "kstring.h"
#include "kerrno.h"
#include "poll.h"

// All of these talk to ourselves: a segment for our own address comes
// straight back in, so both ends of a connection are in this kernel.

// --- A server task that echoes one connection --------------------------------

static tcp_socket_t *echo_listener;
static volatile int echo_served;
static volatile uint64_t echo_bytes;

// Accepts one connection on echo_listener and sends back all it receives
static void echo_server_entry(void) {
    int error = 0;
    tcp_socket_t *conn = tcp_accept(echo_listener, &error);
    if (conn) {
        static uint8_t buf[2048];
        int64_t n;
        while ((n = tcp_read(conn, buf, sizeof(buf))) > 0) {
            if (tcp_write(conn, buf, (size_t)n) != n) break;
            echo_bytes += (uint64_t)n;
        }
        tcp_close(conn);
    }
    echo_served++;
}

// Listen on `port` and start the echo task. Returns the listener, or NULL
static tcp_socket_t *start_echo_server(uint16_t port) {
    echo_served = 0;
    echo_bytes = 0;
    echo_listener = tcp_open();
    if (!echo_listener) return NULL;
    if (tcp_bind(echo_listener, port) != 0 || tcp_listen(echo_listener, 4) != 0) return NULL;
    task_create("tcp_echo", echo_server_entry);
    return echo_listener;
}

// Wait (up to 6 s) until only `count` sockets are left
static void wait_for_sockets_to_go(size_t count) {
    // Closing finishes in the background (the last acknowledgements, then
    // two seconds in TIME_WAIT)
    TEST_WAIT(tcp_sockets_in_use() == count, 600);
}

// Read until `want` bytes have arrived or the connection ends; *got says how many
static void read_fully(tcp_socket_t *sock, uint8_t *buf, size_t want, size_t *got) {
    *got = 0;
    while (*got < want) {
        int64_t n = tcp_read(sock, buf + *got, want - *got);
        if (n <= 0) break;
        *got += (size_t)n;
    }
}

TEST(tcp_connect_exchange_close) {
    size_t sockets_before = tcp_sockets_in_use();
    size_t heap_before = kheap_get_allocated_bytes();
    sched_reap();

    EXPECT_TRUE(start_echo_server(7100) != NULL);

    tcp_socket_t *client = tcp_open();
    EXPECT_TRUE(client != NULL);
    EXPECT_EQ(tcp_read(client, &(uint8_t){0}, 1), -ENOTCONN);       // Not connected yet
    EXPECT_EQ(tcp_write(client, "x", 1), -ENOTCONN);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7100), 0);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7100), -EISCONN);

    uint32_t peer_ip = 0; uint16_t peer_port = 0;
    tcp_peer(client, &peer_ip, &peer_port);
    EXPECT_TRUE(peer_ip == NET_LOCAL_IP && peer_port == 7100);

    uint8_t buf[64];
    size_t got = 0;
    EXPECT_EQ(tcp_write(client, "hello, tcp", 10), 10);
    read_fully(client, buf, 10, &got);
    EXPECT_EQ(got, (size_t)10);
    EXPECT_EQ(memcmp(buf, "hello, tcp", 10), 0);

    // We finish sending: the server reads end-of-stream, closes its side,
    // and we read end-of-stream in turn
    EXPECT_EQ(tcp_shutdown(client), 0);
    EXPECT_EQ(tcp_write(client, "more", 4), -EPIPE);
    EXPECT_EQ(tcp_read(client, buf, sizeof(buf)), 0);
    EXPECT_EQ(tcp_read(client, buf, sizeof(buf)), 0);                // And again
    tcp_close(client);

    TEST_WAIT(echo_served == 1, 300);
    EXPECT_EQ(echo_served, 1);
    EXPECT_TRUE(echo_bytes == 10);
    tcp_close(echo_listener);

    // Nothing is left behind once the close has run its course
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
    while (sched_reap() == 0) sched_yield();
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

TEST(tcp_refuses_resets_and_rejects) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t resets_before = tcp_stats()->resets_sent;

    // Nobody listens there: the answer is a reset, and connect says so
    tcp_socket_t *client = tcp_open();
    EXPECT_TRUE(client != NULL);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7199), -ECONNREFUSED);
    EXPECT_TRUE(tcp_stats()->resets_sent > resets_before);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7199), -EISCONN);   // A failed socket is not reused
    tcp_close(client);

    // Argument checks
    tcp_socket_t *a = tcp_open(), *b = tcp_open();
    EXPECT_TRUE(a && b);
    EXPECT_EQ(tcp_listen(a, 1), -EINVAL);                           // Not bound
    EXPECT_EQ(tcp_bind(a, 0), -EINVAL);
    EXPECT_EQ(tcp_bind(a, 7101), 0);
    EXPECT_EQ(tcp_bind(a, 7102), -EINVAL);                          // Already bound
    EXPECT_EQ(tcp_bind(b, 7101), -EADDRINUSE);
    EXPECT_EQ(tcp_listen(a, 1), 0);
    EXPECT_EQ(tcp_connect(a, NET_LOCAL_IP, 7100), -EISCONN);        // A listening socket does not connect
    EXPECT_EQ(tcp_shutdown(a), -ENOTCONN);
    EXPECT_EQ(tcp_connect(b, 0, 80), -EINVAL);
    EXPECT_EQ(tcp_connect(b, NET_LOCAL_IP, 0), -EINVAL);
    int error = 0;
    EXPECT_TRUE(tcp_accept(b, &error) == NULL && error == -EINVAL); // Not listening

    // A connection that is made but never accepted is dealt with when the
    // listening socket closes; the client sees the end of the stream
    EXPECT_EQ(tcp_connect(b, NET_LOCAL_IP, 7101), 0);
    tcp_close(a);
    uint8_t byte;
    EXPECT_EQ(tcp_read(b, &byte, 1), 0);
    tcp_close(b);

    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// The other end may start sending the moment the connection is made, before
// the server has got round to accepting it. That data must be there when it
// does.
TEST(tcp_keeps_data_sent_before_accept) {
    size_t sockets_before = tcp_sockets_in_use();
    tcp_socket_t *listener = tcp_open();
    tcp_socket_t *client = tcp_open();
    EXPECT_TRUE(listener && client);
    EXPECT_EQ(tcp_bind(listener, 7106), 0);
    EXPECT_EQ(tcp_listen(listener, 1), 0);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7106), 0);
    EXPECT_EQ(tcp_write(client, "early bird", 10), 10);
    EXPECT_EQ(tcp_shutdown(client), 0);
    sched_sleep_ticks(10);                       // All of it, and the FIN, arrive unaccepted

    int error = 0;
    tcp_socket_t *conn = tcp_accept(listener, &error);
    EXPECT_TRUE(conn != NULL);
    uint8_t buf[32];
    size_t got = 0;
    read_fully(conn, buf, sizeof(buf), &got);
    EXPECT_EQ(got, (size_t)10);
    EXPECT_EQ(memcmp(buf, "early bird", 10), 0);

    tcp_close(conn);
    tcp_close(client);
    tcp_close(listener);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// --- A large transfer, with and without loss -----------------------------------

#define TRANSFER_BYTES (300 * 1024)

static tcp_socket_t *sink_listener;
static volatile int sink_done;
static volatile uint64_t sink_bytes, sink_errors;

// The byte at `position` of the test stream (not periodic over a window)
static uint8_t pattern_byte(uint64_t position) {
    return (uint8_t)((position * 31 + (position >> 8) * 7) & 0xFF);
}

// Reads everything, slowly at first so that the sender meets a full window
static void sink_entry(void) {
    int error = 0;
    tcp_socket_t *conn = tcp_accept(sink_listener, &error);
    if (conn) {
        sched_sleep_ticks(30);
        static uint8_t buf[3000];
        int64_t n;
        while ((n = tcp_read(conn, buf, sizeof(buf))) > 0) {
            for (int64_t i = 0; i < n; i++) {
                if (buf[i] != pattern_byte(sink_bytes + (uint64_t)i)) sink_errors++;
            }
            sink_bytes += (uint64_t)n;
        }
        if (n < 0) sink_errors++;
        tcp_close(conn);
    }
    sink_done = 1;
}

// Send TRANSFER_BYTES of the pattern to a sink task over a loopback
// connection on `port`, and check that all of it arrived intact
static void transfer(uint16_t port) {
    sink_done = 0; sink_bytes = 0; sink_errors = 0;
    sink_listener = tcp_open();
    EXPECT_TRUE(sink_listener != NULL);
    EXPECT_EQ(tcp_bind(sink_listener, port), 0);
    EXPECT_EQ(tcp_listen(sink_listener, 1), 0);
    EXPECT_TRUE(task_create("tcp_sink", sink_entry) != NULL);

    uint64_t started_ns = clock_ns();
    tcp_socket_t *client = tcp_open();
    EXPECT_TRUE(client != NULL);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, port), 0);

    // /proc/tcp: the client, connected and nothing sent, at the initial
    // window (3 segments of 1460, RFC 5681 eq. 1) and ssthresh "high"
    static char text[4096];
    char expected[48];
    tcp_describe(text, sizeof(text));
    // (ssthresh is 65535 in the other end's units: both scale by 2^3)
    ksnprintf(expected, sizeof(expected), ":%u  established  4380  524280  ", (unsigned)port);
    EXPECT_TRUE(strstr(text, expected) != NULL);
    if (!strstr(text, expected)) kprintf("%s", text);

    static uint8_t chunk[5000];
    uint64_t sent = 0;
    while (sent < TRANSFER_BYTES) {
        size_t n = TRANSFER_BYTES - sent < sizeof(chunk) ? (size_t)(TRANSFER_BYTES - sent) : sizeof(chunk);
        for (size_t i = 0; i < n; i++) chunk[i] = pattern_byte(sent + i);
        int64_t w = tcp_write(client, chunk, n);
        if (w != (int64_t)n) break;
        sent += n;
    }
    EXPECT_TRUE(sent == TRANSFER_BYTES);
    tcp_close(client);   // What is still queued is sent before the connection closes

    TEST_WAIT(sink_done, 6000);
    uint64_t elapsed_ms = (clock_ns() - started_ns) / 1000000;
    EXPECT_EQ(sink_done, 1);
    EXPECT_TRUE(sink_bytes == TRANSFER_BYTES);
    EXPECT_TRUE(sink_errors == 0);
    tcp_close(sink_listener);
    const tcp_stats_t *st = tcp_stats();
    kprintf("    %lu ms; %lu segments out, %lu in, %lu retransmissions, %lu fast, %lu recoveries (%lu partial acks), "
            "%lu out of order (%lu kept), %lu round trips, %lu resets\n",
            elapsed_ms, st->segments_out, st->segments_in, st->retransmissions, st->fast_retransmissions, st->recoveries,
            st->partial_acks, st->out_of_order, st->out_of_order_kept, st->rtt_samples, st->resets_sent);
}

static uint32_t cwnd_trace[TCP_TRACE_MAX];

// Print the start of a congestion window trace, in segments of 1460 bytes
// (rounded down), on one line
static void print_trace(size_t n) {
    kprintf("    cwnd (segments):");
    for (size_t i = 0; i < n && i < 48; i++) kprintf(" %u", cwnd_trace[i] / 1460);
    kprintf(n > 48 ? " ... (%zu values)\n" : " (%zu values)\n", n);
}

TEST(tcp_large_transfer_arrives_intact) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t samples_before = tcp_stats()->rtt_samples;
    tcp_test_trace_cwnd(7103);
    transfer(7103);
    tcp_test_trace_cwnd(0);
    EXPECT_TRUE(tcp_stats()->rtt_samples > samples_before + 10);  // Round trips are measured

    // Slow start: from the initial window, one segment more per
    // acknowledgement, well past it
    size_t n = tcp_test_cwnd_trace(cwnd_trace, TCP_TRACE_MAX);
    print_trace(n);
    EXPECT_TRUE(n > 3 && cwnd_trace[0] == 3 * 1460);
    EXPECT_TRUE(n > 3 && cwnd_trace[1] > cwnd_trace[0] && cwnd_trace[1] <= 4 * 1460);
    uint32_t largest = 0;
    for (size_t i = 0; i < n; i++) if (cwnd_trace[i] > largest) largest = cwnd_trace[i];
    EXPECT_TRUE(largest > 6 * 3 * 1460);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// One segment in seven (data, acknowledgement, SYN or FIN alike) is thrown
// away instead of sent. Everything must still arrive, once, in order.
TEST(tcp_survives_lost_segments) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t retransmissions_before = tcp_stats()->retransmissions;
    uint64_t dropped_before = tcp_stats()->dropped_for_test;
    uint64_t fast_before = tcp_stats()->fast_retransmissions;
    uint64_t kept_before = tcp_stats()->out_of_order_kept;

    uint64_t recoveries_before = tcp_stats()->recoveries;
    tcp_test_drop_every(7);
    tcp_test_trace_cwnd(7104);
    transfer(7104);
    tcp_test_trace_cwnd(0);
    wait_for_sockets_to_go(sockets_before);
    tcp_test_drop_every(0);

    // The congestion window falls at a loss and grows again after it, and
    // never below one segment
    size_t n = tcp_test_cwnd_trace(cwnd_trace, TCP_TRACE_MAX);
    print_trace(n);
    size_t fall = 0;
    for (size_t i = 1; i < n && !fall; i++) if (cwnd_trace[i] < cwnd_trace[i - 1]) fall = i;
    EXPECT_TRUE(fall > 0);
    bool grew_again = false;
    for (size_t i = fall + 1; i < n; i++) if (cwnd_trace[i] > cwnd_trace[fall]) grew_again = true;
    EXPECT_TRUE(grew_again);
    for (size_t i = 0; i < n; i++) EXPECT_TRUE(cwnd_trace[i] >= 1460 && cwnd_trace[i] <= TCP_CWND_MAX);
    EXPECT_TRUE(tcp_stats()->recoveries > recoveries_before + 5);   // Fast recovery, not only timeouts

    EXPECT_TRUE(tcp_stats()->dropped_for_test > dropped_before + 20);
    // Losses are mended, mostly without waiting for the timer: segments
    // after a lost one are kept, and their duplicate acknowledgements bring
    // the lost one back at once
    EXPECT_TRUE(tcp_stats()->fast_retransmissions + tcp_stats()->retransmissions > fast_before + retransmissions_before);
    EXPECT_TRUE(tcp_stats()->fast_retransmissions > fast_before + 5);
    EXPECT_TRUE(tcp_stats()->out_of_order_kept > kept_before + 5);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// The other end stops answering altogether: the connection is given up
// rather than waited on for ever, and the blocked reader is told. (The other
// end here is a connection the listening socket made and nobody accepted.)
TEST(tcp_gives_up_on_a_dead_peer) {
    size_t sockets_before = tcp_sockets_in_use();
    tcp_socket_t *listener = tcp_open();
    tcp_socket_t *client = tcp_open();
    EXPECT_TRUE(listener && client);
    EXPECT_EQ(tcp_bind(listener, 7105), 0);
    EXPECT_EQ(tcp_listen(listener, 1), 0);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7105), 0);

    tcp_test_drop_every(1); // Nothing gets through any more, in either direction
    EXPECT_EQ(tcp_write(client, "anyone?", 7), 7);
    uint8_t buf[8];
    uint64_t start = timer_ticks();
    EXPECT_EQ(tcp_read(client, buf, sizeof(buf)), -ETIMEDOUT);
    EXPECT_TRUE(timer_ticks() - start > 100);   // After several tries, not at once
    EXPECT_EQ(tcp_write(client, "x", 1), -ETIMEDOUT);
    tcp_test_drop_every(0);
    tcp_close(client);

    // The other side never noticed (it had nothing to send). When its owner
    // closes it, its FIN finds nobody, the answer is a reset, and it goes.
    tcp_close(listener);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// The list of stretches kept beyond the gap: sorted, joined where they touch
// or overlap, refusing (and changing nothing) when it would need too many,
// across the wrap of sequence numbers
TEST(tcp_reassembly_list) {
    tcp_ooo_t q = { .count = 0 };
    EXPECT_TRUE(tcp_ooo_add(&q, 300, 400));
    EXPECT_TRUE(tcp_ooo_add(&q, 100, 200));                       // Before it: sorted
    EXPECT_EQ(q.count, 2u);
    EXPECT_TRUE(q.range[0].start == 100 && q.range[1].start == 300);
    EXPECT_TRUE(tcp_ooo_add(&q, 200, 300));                       // Touches both: one stretch
    EXPECT_EQ(q.count, 1u);
    EXPECT_TRUE(q.range[0].start == 100 && q.range[0].end == 400);
    EXPECT_TRUE(tcp_ooo_add(&q, 150, 250));                       // Inside: nothing new
    EXPECT_TRUE(tcp_ooo_add(&q, 50, 450));                        // Over it: grows
    EXPECT_TRUE(q.count == 1 && q.range[0].start == 50 && q.range[0].end == 450);

    // Not reached yet; then reached part way; then past
    EXPECT_EQ(tcp_ooo_take(&q, 10), 10u);
    EXPECT_EQ(q.count, 1u);
    EXPECT_EQ(tcp_ooo_take(&q, 60), 450u);                        // The rest is in: next is its end
    EXPECT_EQ(q.count, 0u);

    // Full: one more separate stretch is refused and the list is as it was
    for (uint32_t i = 0; i < TCP_OOO_RANGES; i++) EXPECT_TRUE(tcp_ooo_add(&q, 1000 + i * 100, 1050 + i * 100));
    tcp_ooo_t before = q;
    EXPECT_TRUE(!(tcp_ooo_add(&q, 5000, 5010)));
    EXPECT_TRUE(!(tcp_ooo_add(&q, 10, 20)));
    EXPECT_EQ(memcmp(&q, &before, sizeof(q)), 0);
    EXPECT_TRUE(tcp_ooo_add(&q, 1040, 1110));                     // Joins two: room again
    EXPECT_EQ(q.count, (uint32_t)TCP_OOO_RANGES - 1);
    EXPECT_TRUE(tcp_ooo_add(&q, 5000, 5010));
    EXPECT_EQ(tcp_ooo_take(&q, 1000), 1150u);                     // Two joined stretches
    EXPECT_EQ(q.count, (uint32_t)TCP_OOO_RANGES - 1);
    EXPECT_EQ(tcp_ooo_take(&q, 1250), 1250u);                     // Exactly at the end of one: it goes, and moves nothing
    EXPECT_EQ(q.count, (uint32_t)TCP_OOO_RANGES - 2);

    // Across 2^32
    tcp_ooo_t w = { .count = 0 };
    EXPECT_TRUE(tcp_ooo_add(&w, 0xFFFFFF00u, 0x00000010u));
    EXPECT_TRUE(tcp_ooo_add(&w, 0xFFFFFE00u, 0xFFFFFE80u));
    EXPECT_TRUE(w.count == 2 && w.range[0].start == 0xFFFFFE00u);
    EXPECT_EQ(tcp_ooo_take(&w, 0xFFFFFE80u), 0xFFFFFE80u);
    EXPECT_EQ(tcp_ooo_take(&w, 0xFFFFFF00u), 0x10u);
    EXPECT_EQ(w.count, 0u);
}

// RFC 6298's arithmetic, checked against the formulas worked by hand
TEST(tcp_rto_follows_rfc6298) {
    tcp_rtt_t t = { 0 };
    // 2.2: SRTT = R, RTTVAR = R/2, RTO = SRTT + max(G, 4 RTTVAR) = 3R
    tcp_rtt_sample(&t, 100000);
    EXPECT_TRUE(t.measured);
    EXPECT_EQ(t.srtt_us, 100000u);
    EXPECT_EQ(t.rttvar_us, 50000u);
    EXPECT_EQ(t.rto_us, 300000u);
    // 2.3: RTTVAR = 3/4 * 50000 + 1/4 * |100000 - 20000| = 57500;
    // SRTT = 7/8 * 100000 + 1/8 * 20000 = 90000; RTO = 90000 + 230000
    tcp_rtt_sample(&t, 20000);
    EXPECT_EQ(t.rttvar_us, 57500u);
    EXPECT_EQ(t.srtt_us, 90000u);
    EXPECT_EQ(t.rto_us, 320000u);

    // Steady small round trips: RTTVAR decays, and the floor holds
    tcp_rtt_t s = { 0 };
    for (int i = 0; i < 50; i++) tcp_rtt_sample(&s, 1000);
    EXPECT_TRUE(s.srtt_us >= 990 && s.srtt_us <= 1000);
    EXPECT_EQ(s.rto_us, TCP_RTO_MIN_US);
    // The granularity term: SRTT + G when 4 RTTVAR is below it (above the floor)
    tcp_rtt_t g = { .srtt_us = 500000, .rttvar_us = 100, .measured = true };
    tcp_rtt_sample(&g, 500000);
    EXPECT_EQ(g.rto_us, 500000u + TCP_CLOCK_GRANULARITY_US);
    // The cap
    tcp_rtt_t c = { 0 };
    tcp_rtt_sample(&c, 5000000);
    EXPECT_EQ(c.rto_us, TCP_RTO_MAX_US);
}

// The congestion window's arithmetic against RFC 5681 (eq. 1 to 4, and
// 3.2) and RFC 6582 (NewReno), worked by hand
TEST(tcp_congestion_window_follows_rfc5681) {
    tcp_cc_t cc;
    // The initial window (eq. 1): 4, 3 or 2 segments as they are small or large
    tcp_cc_init(&cc, 536, 1000);
    EXPECT_EQ(cc.cwnd, 4u * 536);
    tcp_cc_init(&cc, 2200, 1000);
    EXPECT_EQ(cc.cwnd, 2u * 2200);
    tcp_cc_init(&cc, 1460, 1000);
    EXPECT_EQ(cc.cwnd, 3u * 1460);
    EXPECT_EQ(cc.ssthresh, 65535u);
    EXPECT_TRUE(!cc.in_recovery && cc.recover == 1000);

    // Slow start (eq. 2): what was acknowledged, at most a segment each time
    EXPECT_TRUE(!tcp_cc_new_ack(&cc, 1460, 1460, 2461, 0));
    EXPECT_EQ(cc.cwnd, 5840u);
    tcp_cc_new_ack(&cc, 1460, 4000, 6461, 0);
    EXPECT_EQ(cc.cwnd, 7300u);
    tcp_cc_new_ack(&cc, 1460, 100, 6561, 0);
    EXPECT_EQ(cc.cwnd, 7400u);
    // Congestion avoidance (eq. 3): SMSS * SMSS / cwnd each time
    cc.cwnd = cc.ssthresh = 14600;
    tcp_cc_new_ack(&cc, 1460, 1460, 8021, 0);
    EXPECT_EQ(cc.cwnd, 14600u + 146);

    // Three duplicates: fast retransmit, ssthresh half the flight (eq. 4),
    // the window inflated by the three (3.2, 3) and by each one after (3.2, 4)
    tcp_cc_init(&cc, 1460, 1000);
    cc.cwnd = 14600;
    EXPECT_TRUE(!tcp_cc_dup_ack(&cc, 1460, 1, 5000, 20001, 15001));
    EXPECT_TRUE(!tcp_cc_dup_ack(&cc, 1460, 2, 5000, 20001, 15001));
    EXPECT_EQ(cc.cwnd, 14600u);
    EXPECT_TRUE(tcp_cc_dup_ack(&cc, 1460, 3, 5000, 20001, 15001));
    EXPECT_EQ(cc.ssthresh, 7500u);
    EXPECT_EQ(cc.cwnd, 7500u + 3 * 1460);
    EXPECT_TRUE(cc.in_recovery && cc.recover == 20000);
    EXPECT_TRUE(!tcp_cc_dup_ack(&cc, 1460, 4, 5000, 20001, 15001));
    EXPECT_EQ(cc.cwnd, 7500u + 4 * 1460);
    // Partial acknowledgements (RFC 6582 3.2, 5): send the next hole, take
    // off what was acknowledged, give back a segment if that much was
    EXPECT_TRUE(tcp_cc_new_ack(&cc, 1460, 2920, 7920, 12081));
    EXPECT_EQ(cc.cwnd, 7500u + 4 * 1460 - 2920 + 1460);
    EXPECT_TRUE(tcp_cc_new_ack(&cc, 1460, 500, 8420, 11581));
    EXPECT_EQ(cc.cwnd, 7500u + 4 * 1460 - 2920 + 1460 - 500);
    EXPECT_TRUE(cc.in_recovery);
    // The full acknowledgement (3.2, 6): min(ssthresh, max(FlightSize, SMSS) + SMSS)
    EXPECT_TRUE(!tcp_cc_new_ack(&cc, 1460, 11581, 20001, 0));
    EXPECT_EQ(cc.cwnd, 2u * 1460);
    EXPECT_TRUE(!cc.in_recovery);
    EXPECT_TRUE(tcp_cc_dup_ack(&cc, 1460, 3, 20001, 40001, 20000));
    EXPECT_TRUE(!tcp_cc_new_ack(&cc, 1460, 20000, 40001, 30000));
    EXPECT_EQ(cc.cwnd, 10000u);                                      // ssthresh, the smaller

    // A timeout: one segment; ssthresh half the flight at the first expiry
    // for a segment only, and never below two segments
    tcp_cc_timeout(&cc, 1460, 20000, 60001, true);
    EXPECT_EQ(cc.cwnd, 1460u);
    EXPECT_EQ(cc.ssthresh, 10000u);
    tcp_cc_timeout(&cc, 1460, 1000, 60001, false);
    EXPECT_EQ(cc.ssthresh, 10000u);
    tcp_cc_timeout(&cc, 1460, 1000, 60001, true);
    EXPECT_EQ(cc.ssthresh, 2u * 1460);
    EXPECT_TRUE(!cc.in_recovery && cc.recover == 60000);
    // Duplicates of what was sent before the timeout start nothing (RFC
    // 6582 3.2, 2); once that is all acknowledged, they do
    EXPECT_TRUE(!tcp_cc_dup_ack(&cc, 1460, 3, 50000, 60001, 10001));
    EXPECT_TRUE(!cc.in_recovery && cc.cwnd == 1460);
    EXPECT_TRUE(tcp_cc_dup_ack(&cc, 1460, 3, 60001, 70001, 10000));

    // Sequence numbers that wrap: beyond recover across 2^32
    tcp_cc_init(&cc, 1460, 0xFFFFFF00u);
    EXPECT_TRUE(tcp_cc_dup_ack(&cc, 1460, 3, 0x10, 0x2000, 0x1FF0));
    // The window grows no further than TCP_CWND_MAX
    tcp_cc_init(&cc, 1460, 0);
    cc.ssthresh = TCP_CWND_MAX;
    cc.cwnd = TCP_CWND_MAX - 10;
    tcp_cc_new_ack(&cc, 1460, 1460, 1461, 0);
    EXPECT_EQ(cc.cwnd, TCP_CWND_MAX);
}

// --- Segments made by hand, to put them in the order a test wants ------------

static uint16_t be16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t be32(uint32_t v) { return (v << 24) | ((v << 8) & 0xFF0000) | ((v >> 8) & 0xFF00) | (v >> 24); }

// Deliver a segment to tcp_input as if from NET_LOCAL_IP:from_port to
// to_port, carrying `len` bytes of `data`
static void inject(uint16_t from_port, uint16_t to_port, uint32_t seq, uint32_t ack, uint8_t flags,
                   const void *data, size_t len) {
    static uint8_t segment[20 + 1500];
    memset(segment, 0, 20);
    *(uint16_t *)(segment + 0) = be16(from_port);
    *(uint16_t *)(segment + 2) = be16(to_port);
    *(uint32_t *)(segment + 4) = be32(seq);
    *(uint32_t *)(segment + 8) = be32(ack);
    segment[12] = 0x50;
    segment[13] = flags;
    *(uint16_t *)(segment + 14) = be16(8192);
    memcpy(segment + 20, data, len);
    *(uint16_t *)(segment + 16) = be16(net_transport_checksum(IP_PROTO_TCP, NET_LOCAL_IP, NET_LOCAL_IP, segment, 20 + len));
    tcp_input(NET_LOCAL_IP, NET_LOCAL_IP, segment, 20 + len);
}

// Data that arrives in the wrong order is kept and read in the right one,
// with a FIN that came ahead of a gap counted only once the gap is filled.
// The peer is played by hand: segments from port 40107, where nothing
// listens, are put straight into the input in the order the test wants,
// while everything the server sends is dropped (it would only meet a reset).
// At the end the server's FIN gets through, and the reset that answers it
// ends the connection.
TEST(tcp_reassembles_out_of_order_segments) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t kept_before = tcp_stats()->out_of_order_kept;
    tcp_socket_t *listener = tcp_open();
    EXPECT_TRUE(listener != NULL);
    EXPECT_EQ(tcp_bind(listener, 7107), 0);
    EXPECT_EQ(tcp_listen(listener, 1), 0);

    const uint16_t peer = 40107;
    const uint32_t s = 0xFFFFFFF0u;              // The peer's first sequence number: the stream wraps
    uint32_t rcv_nxt = 0, ack = 0, iss = 0;
    tcp_test_drop_every(1);
    inject(peer, 7107, s, 0, 0x02, NULL, 0);                      // SYN
    EXPECT_TRUE(tcp_test_sequences(7107, peer, &rcv_nxt, &ack, &iss));
    EXPECT_EQ(rcv_nxt, s + 1);
    // The SYN+ACK goes out from tcpd, which on another CPU may already have
    // sent it: the initial sequence number is the unacknowledged one, sent
    // or not (taking it from what had been sent was off by one on 4 CPUs)
    TEST_WAIT(tcp_test_sequences(7107, peer, &rcv_nxt, &ack, &iss) && ack == iss + 1, 100);
    EXPECT_EQ(ack, iss + 1);
    inject(peer, 7107, s + 1, ack, 0x10, NULL, 0);                // ACK of its SYN+ACK
    int error = 0;
    tcp_socket_t *server = tcp_accept(listener, &error);
    EXPECT_TRUE(server != NULL);
    if (!server) { tcp_test_drop_every(0); return; }

    // "abcdefghij" in pieces: the last first, a FIN beyond the gap, the
    // middle, a repeat of part of what is kept; nothing is readable and
    // nothing is counted as received until the first piece fills the gap
    uint32_t next = s + 1;
    inject(peer, 7107, next + 7, ack, 0x10, "hij", 3);
    inject(peer, 7107, next + 10, ack, 0x11, NULL, 0);
    inject(peer, 7107, next + 3, ack, 0x10, "defg", 4);
    inject(peer, 7107, next + 5, ack, 0x10, "fg", 2);
    EXPECT_TRUE(tcp_test_sequences(7107, peer, &rcv_nxt, &ack, &iss) && rcv_nxt == next);
    EXPECT_TRUE(!(tcp_poll(server) & POLLIN));
    inject(peer, 7107, next, ack, 0x10, "abc", 3);
    EXPECT_TRUE(tcp_test_sequences(7107, peer, &rcv_nxt, &ack, &iss) && rcv_nxt == next + 11); // Ten bytes and the FIN
    uint8_t buf[64];
    size_t got = 0;
    // (Read only if the stream is complete: otherwise the read would wait
    // for ever for a peer that is the test itself)
    if (rcv_nxt == next + 11) read_fully(server, buf, sizeof(buf), &got);
    EXPECT_EQ(got, (size_t)10);
    EXPECT_EQ(memcmp(buf, "abcdefghij", 10), 0);
    EXPECT_TRUE(got == 10 && tcp_read(server, buf, 1) == 0);    // Then the end of the stream
    EXPECT_TRUE(tcp_stats()->out_of_order_kept >= kept_before + 2);

    tcp_close(server);
    tcp_close(listener);
    tcp_test_drop_every(0);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// A listener that closes while a connection is half open (its SYN+ACK sent,
// the ACK of it not yet in) must not forget it: the other end already
// counts itself connected and would get a reset for its ACK instead of the
// end of the stream. (On four CPUs tcp_connect returned before tcpd had
// sent that ACK, and tcp_refuses_resets_and_rejects read ECONNRESET.) The
// peer is played by hand from port 40109, everything sent to it dropped.
TEST(tcp_listener_close_finishes_half_open_connections) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t resets_before = tcp_stats()->resets_sent;
    tcp_socket_t *listener = tcp_open();
    EXPECT_TRUE(listener != NULL);
    EXPECT_EQ(tcp_bind(listener, 7109), 0);
    EXPECT_EQ(tcp_listen(listener, 1), 0);

    const uint16_t peer = 40109;
    const uint32_t s = 9000;                     // The peer's first sequence number
    uint32_t rcv_nxt = 0, sent = 0, iss = 0;
    tcp_test_drop_every(1);
    inject(peer, 7109, s, 0, 0x02, NULL, 0);                        // SYN
    TEST_WAIT(tcp_test_sequences(7109, peer, &rcv_nxt, &sent, &iss) && sent == iss + 1, 100);
    EXPECT_EQ(sent, iss + 1);                                       // SYN+ACK out
    tcp_close(listener);
    EXPECT_TRUE(tcp_test_sequences(7109, peer, &rcv_nxt, &sent, &iss)); // Still there

    const uint32_t first = iss;                  // (From here on `iss` is the unacknowledged one)
    inject(peer, 7109, s + 1, first + 1, 0x10, NULL, 0);           // ACK of the SYN+ACK
    TEST_WAIT(tcp_test_sequences(7109, peer, &rcv_nxt, &sent, &iss) && sent == first + 2, 100);
    EXPECT_EQ(sent, first + 2);                                     // ... answered by a FIN
    EXPECT_TRUE(tcp_stats()->resets_sent == resets_before);         // and not by a reset

    inject(peer, 7109, s + 1, 0, 0x04, NULL, 0);                    // A reset from the peer ends it
    tcp_test_drop_every(0);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// Malformed and stray segments must be counted and dropped, not acted on
TEST(tcp_rejects_bad_segments) {
    uint64_t bad_before = tcp_stats()->bad_segments;
    uint8_t segment[40];

    memset(segment, 0, sizeof(segment));
    tcp_input(NET_LOCAL_IP, NET_LOCAL_IP, segment, 10);              // Shorter than a header
    segment[12] = 0x20;                                               // Header length 8 bytes: impossible
    tcp_input(NET_LOCAL_IP, NET_LOCAL_IP, segment, 20);
    segment[12] = 0xF0;                                               // Header longer than the segment
    tcp_input(NET_LOCAL_IP, NET_LOCAL_IP, segment, 20);
    segment[12] = 0x50;                                               // Well formed, wrong checksum
    segment[16] = 0x12; segment[17] = 0x34;
    tcp_input(NET_LOCAL_IP, NET_LOCAL_IP, segment, 20);
    EXPECT_TRUE(tcp_stats()->bad_segments == bad_before + 4);
}

// The sender keeps to the congestion window, not only to the receiver's.
// The peer is played by hand (from port 40108; everything the server sends
// is thrown away) and offers 8192 bytes of window; its SYN names no segment
// size, so segments are 536 bytes and the initial window four of them
// (RFC 5681 eq. 1). Of the 8000 bytes the server has to send, only those
// four go out; an acknowledgement of one grows the window by one segment
// (slow start), so two more go; three duplicate acknowledgements then bring
// a fast retransmit and start a recovery. (What the server has sent is read
// from its sequence numbers, which only it changes, not from timing.)
TEST(tcp_sender_keeps_to_the_congestion_window) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t fast_before = tcp_stats()->fast_retransmissions;
    uint64_t recoveries_before = tcp_stats()->recoveries;
    tcp_socket_t *listener = tcp_open();
    EXPECT_TRUE(listener != NULL);
    EXPECT_EQ(tcp_bind(listener, 7108), 0);
    EXPECT_EQ(tcp_listen(listener, 1), 0);

    const uint16_t peer = 40108;
    const uint32_t s = 5000;                     // The peer's first sequence number
    uint32_t rcv_nxt = 0, sent = 0, iss = 0;
    tcp_test_drop_every(1);
    inject(peer, 7108, s, 0, 0x02, NULL, 0);                        // SYN, no options
    EXPECT_TRUE(tcp_test_sequences(7108, peer, &rcv_nxt, &sent, &iss));
    TEST_WAIT(tcp_test_sequences(7108, peer, &rcv_nxt, &sent, &iss) && sent == iss + 1, 100);
    inject(peer, 7108, s + 1, iss + 1, 0x10, NULL, 0);             // ACK of the SYN+ACK
    int error = 0;
    tcp_socket_t *server = tcp_accept(listener, &error);
    EXPECT_TRUE(server != NULL);
    if (!server) { tcp_test_drop_every(0); tcp_close(listener); return; }

    static uint8_t data[8000];
    memset(data, 'w', sizeof(data));
    EXPECT_EQ(tcp_write(server, data, sizeof(data)), (int64_t)sizeof(data));
    const uint32_t first = iss + 1;
    TEST_WAIT(tcp_test_sequences(7108, peer, &rcv_nxt, &sent, &iss) && sent == first + 4 * 536, 100);
    sched_sleep_ticks(5);                       // Nothing more follows (the receiver would take 8192)
    EXPECT_TRUE(tcp_test_sequences(7108, peer, &rcv_nxt, &sent, &iss));
    EXPECT_EQ(sent, first + 4 * 536);

    inject(peer, 7108, s + 1, first + 536, 0x10, NULL, 0);          // One segment acknowledged
    TEST_WAIT(tcp_test_sequences(7108, peer, &rcv_nxt, &sent, &iss) && sent == first + 536 + 5 * 536, 100);
    sched_sleep_ticks(5);
    EXPECT_TRUE(tcp_test_sequences(7108, peer, &rcv_nxt, &sent, &iss));
    EXPECT_EQ(sent, first + 536 + 5 * 536);

    for (int i = 0; i < 3; i++) inject(peer, 7108, s + 1, first + 536, 0x10, NULL, 0);
    TEST_WAIT(tcp_stats()->fast_retransmissions > fast_before, 100);
    EXPECT_TRUE(tcp_stats()->fast_retransmissions == fast_before + 1);
    EXPECT_TRUE(tcp_stats()->recoveries == recoveries_before + 1);

    // The end: what the server sends now reaches port 40108, where nothing
    // listens, and the reset that answers it ends the connection
    tcp_close(server);
    tcp_close(listener);
    tcp_test_drop_every(0);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// --- Window scaling (RFC 7323, section 2) ---------------------------------------

// The options a SYN can carry, well-formed and not
TEST(tcp_options_are_read) {
    tcp_options_t o;
    static const uint8_t both[] = { 2, 4, 0x05, 0xB4, 1, 3, 3, 7 };      // MSS 1460, NOP, scale 7
    tcp_parse_options(both, sizeof(both), &o);
    EXPECT_TRUE(o.has_mss && o.mss == 1460 && o.has_wscale && o.wscale == 7);

    static const uint8_t mss_only[] = { 2, 4, 0x02, 0x18 };             // MSS 536
    tcp_parse_options(mss_only, sizeof(mss_only), &o);
    EXPECT_TRUE(o.has_mss && o.mss == 536 && !o.has_wscale);

    tcp_parse_options(NULL, 0, &o);                                     // No options at all
    EXPECT_TRUE(!o.has_mss && !o.has_wscale);

    static const uint8_t too_big[] = { 3, 3, 15, 0 };                   // Shift 15, then end of list
    tcp_parse_options(too_big, sizeof(too_big), &o);
    EXPECT_TRUE(o.has_wscale && o.wscale == TCP_WSCALE_MAX);           // Taken as 14 (2.3)
    static const uint8_t huge[] = { 3, 3, 255 };
    tcp_parse_options(huge, sizeof(huge), &o);
    EXPECT_TRUE(o.has_wscale && o.wscale == 14);

    // Malformed: the wrong length for the kind, an option running past the
    // end, a length of zero (which would loop for ever), the list's end
    static const uint8_t wrong_len[] = { 3, 4, 2, 0 };
    tcp_parse_options(wrong_len, sizeof(wrong_len), &o);
    EXPECT_TRUE(!o.has_wscale);
    static const uint8_t truncated[] = { 2, 4, 0x05, 0xB4, 3, 3 };
    tcp_parse_options(truncated, sizeof(truncated), &o);
    EXPECT_TRUE(o.has_mss && !o.has_wscale);                          // What came before stands
    static const uint8_t zero_len[] = { 8, 0, 3, 3, 2 };
    tcp_parse_options(zero_len, sizeof(zero_len), &o);
    EXPECT_TRUE(!o.has_wscale);
    static const uint8_t ended[] = { 0, 3, 3, 2 };
    tcp_parse_options(ended, sizeof(ended), &o);
    EXPECT_TRUE(!o.has_wscale);
    static const uint8_t unknown_first[] = { 8, 10, 1, 2, 3, 4, 5, 6, 7, 8, 3, 3, 4 };  // Timestamps, skipped
    tcp_parse_options(unknown_first, sizeof(unknown_first), &o);
    EXPECT_TRUE(o.has_wscale && o.wscale == 4);
}

// A SYN from port `from_port` with the given options (a multiple of 4 bytes)
static void inject_syn(uint16_t from_port, uint16_t to_port, uint32_t seq, const uint8_t *options, size_t options_len) {
    static uint8_t segment[20 + 40];
    memset(segment, 0, sizeof(segment));
    *(uint16_t *)(segment + 0) = be16(from_port);
    *(uint16_t *)(segment + 2) = be16(to_port);
    *(uint32_t *)(segment + 4) = be32(seq);
    segment[12] = (uint8_t)(((20 + options_len) / 4) << 4);
    segment[13] = 0x02;
    *(uint16_t *)(segment + 14) = be16(8192);
    memcpy(segment + 20, options, options_len);
    *(uint16_t *)(segment + 16) = be16(net_transport_checksum(IP_PROTO_TCP, NET_LOCAL_IP, NET_LOCAL_IP, segment, 20 + options_len));
    tcp_input(NET_LOCAL_IP, NET_LOCAL_IP, segment, 20 + options_len);
}

// Scaling is used when both SYNs carry the option and not otherwise; a
// shift above 14 is taken as 14; windows after the SYNs are scaled. The
// peers by hand are on ports 40110 to 40112, everything sent to them dropped.
TEST(tcp_window_scaling_is_negotiated) {
    size_t sockets_before = tcp_sockets_in_use();
    tcp_socket_t *listener = tcp_open();
    EXPECT_TRUE(listener != NULL);
    EXPECT_EQ(tcp_bind(listener, 7110), 0);
    EXPECT_EQ(tcp_listen(listener, 4), 0);

    // Two ends of ours: both offer, both use it
    tcp_socket_t *client = tcp_open();
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7110), 0);
    int error = 0;
    tcp_socket_t *server = tcp_accept(listener, &error);
    EXPECT_TRUE(server != NULL);
    uint32_t ip = 0;
    uint16_t client_port = 0;
    tcp_peer(server, &ip, &client_port);
    tcp_test_windows_t w;
    EXPECT_TRUE(tcp_test_windows(7110, client_port, &w));
    EXPECT_TRUE(w.scaling && w.snd_shift == TCP_WSCALE_OURS && w.rcv_shift == TCP_WSCALE_OURS);
    EXPECT_TRUE(tcp_test_windows(client_port, 7110, &w));
    EXPECT_TRUE(w.scaling && w.snd_shift == TCP_WSCALE_OURS && w.rcv_shift == TCP_WSCALE_OURS);
    // The server's window (8 KiB, sent as 1024 units of 8) arrived as 8 KiB
    EXPECT_EQ(w.snd_wnd, TCP_BUFFER_SIZE);
    tcp_close(client);
    tcp_close(server);

    tcp_test_drop_every(1);
    // A SYN without the option: no scaling either way, and our SYN+ACK
    // does not carry it (RFC 7323 2.2)
    static const uint8_t mss_only[] = { 2, 4, 0x05, 0xB4 };
    inject_syn(40110, 7110, 5000, mss_only, sizeof(mss_only));
    EXPECT_TRUE(tcp_test_windows(7110, 40110, &w));
    EXPECT_TRUE(!w.scaling && w.snd_shift == 0 && w.rcv_shift == 0);
    uint32_t rcv_nxt = 0, sent = 0, iss = 0;
    TEST_WAIT(tcp_test_sequences(7110, 40110, &rcv_nxt, &sent, &iss) && sent == iss + 1, 100);
    inject(40110, 7110, 5001, iss + 1, 0x10, NULL, 0);               // ACK, window 8192
    EXPECT_TRUE(tcp_test_windows(7110, 40110, &w));
    EXPECT_EQ(w.snd_wnd, 8192);                                       // Taken as it is

    // A shift of 15: taken as 14, and the window after the SYN scaled by it
    static const uint8_t scale_15[] = { 2, 4, 0x05, 0xB4, 1, 3, 3, 15 };
    inject_syn(40111, 7110, 6000, scale_15, sizeof(scale_15));
    EXPECT_TRUE(tcp_test_windows(7110, 40111, &w));
    EXPECT_TRUE(w.scaling && w.snd_shift == 14 && w.rcv_shift == TCP_WSCALE_OURS);
    EXPECT_EQ(w.snd_wnd, 8192);                                       // A SYN's window is never scaled
    TEST_WAIT(tcp_test_sequences(7110, 40111, &rcv_nxt, &sent, &iss) && sent == iss + 1, 100);
    inject(40111, 7110, 6001, iss + 1, 0x10, NULL, 0);
    EXPECT_TRUE(tcp_test_windows(7110, 40111, &w));
    EXPECT_EQ(w.snd_wnd, 8192u << 14);

    // Resets from the peers end both
    inject(40110, 7110, 5001, 0, 0x04, NULL, 0);
    inject(40111, 7110, 6001, 0, 0x04, NULL, 0);
    tcp_test_drop_every(0);
    tcp_close(listener);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

#define GROW_BYTES (4 * 1024 * 1024)

static tcp_socket_t *grow_listener;
static volatile int grow_done;
static volatile uint64_t grow_bytes, grow_errors;
static volatile uint16_t grow_peer_port;
static tcp_test_windows_t grow_server_windows;

// Accepts one connection and reads it to the end in large pieces, keeping
// up with the sender; notes the connection's windows before closing it
static void grow_reader_entry(void) {
    int error = 0;
    tcp_socket_t *conn = tcp_accept(grow_listener, &error);
    if (conn) {
        uint32_t ip;
        uint16_t port;
        tcp_peer(conn, &ip, &port);
        grow_peer_port = port;
        static uint8_t buf[65536];
        int64_t n;
        while ((n = tcp_read(conn, buf, sizeof(buf))) > 0) {
            for (int64_t i = 0; i < n; i++) {
                if (buf[i] != pattern_byte(grow_bytes + (uint64_t)i)) grow_errors++;
            }
            grow_bytes += (uint64_t)n;
        }
        if (n < 0) grow_errors++;
        tcp_test_windows(7111, port, &grow_server_windows);
        tcp_close(conn);
    }
    grow_done = 1;
}

// With scaling, a reader that keeps up and a writer with more to send, both
// rings grow past what a 16-bit window could describe, up to TCP_BUFFER_MAX
TEST(tcp_rings_grow_past_64k) {
    size_t sockets_before = tcp_sockets_in_use();
    grow_done = 0; grow_bytes = 0; grow_errors = 0; grow_peer_port = 0;
    grow_listener = tcp_open();
    EXPECT_EQ(tcp_bind(grow_listener, 7111), 0);
    EXPECT_EQ(tcp_listen(grow_listener, 1), 0);
    EXPECT_TRUE(task_create("tcp_grow", grow_reader_entry) != NULL);

    tcp_socket_t *client = tcp_open();
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7111), 0);
    uint64_t started_ns = clock_ns();
    static uint8_t chunk[16384];
    uint64_t sent = 0;
    while (sent < GROW_BYTES) {
        for (size_t i = 0; i < sizeof(chunk); i++) chunk[i] = pattern_byte(sent + i);
        if (tcp_write(client, chunk, sizeof(chunk)) != (int64_t)sizeof(chunk)) break;
        sent += sizeof(chunk);
    }
    EXPECT_TRUE(sent == GROW_BYTES);
    TEST_WAIT(grow_peer_port != 0, 100);
    tcp_test_windows_t w;
    EXPECT_TRUE(tcp_test_windows(grow_peer_port, 7111, &w));
    EXPECT_TRUE(w.tx_size > 65536 && w.tx_size <= TCP_BUFFER_MAX);
    kprintf("    sender's ring %u bytes, ", (unsigned)w.tx_size);
    tcp_close(client);

    TEST_WAIT(grow_done, 6000);
    uint64_t elapsed_ms = (clock_ns() - started_ns) / 1000000;
    EXPECT_EQ(grow_done, 1);
    EXPECT_TRUE(grow_bytes == GROW_BYTES && grow_errors == 0);
    EXPECT_TRUE(grow_server_windows.rx_size > 65536 && grow_server_windows.rx_size <= TCP_BUFFER_MAX);
    kprintf("receiver's %u; %lu ms for %u MiB\n", (unsigned)grow_server_windows.rx_size, elapsed_ms,
            (unsigned)(GROW_BYTES >> 20));
    tcp_close(grow_listener);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

void test_suite_tcp(void) {
    kprintf("Suite: TCP\n");
    if (!net_up()) {
        kprintf("  (no network card: skipped)\n");
        return;
    }
    RUN_TEST(tcp_connect_exchange_close);
    RUN_TEST(tcp_refuses_resets_and_rejects);
    RUN_TEST(tcp_keeps_data_sent_before_accept);
    RUN_TEST(tcp_large_transfer_arrives_intact);
    RUN_TEST(tcp_survives_lost_segments);
    RUN_TEST(tcp_gives_up_on_a_dead_peer);
    RUN_TEST(tcp_listener_close_finishes_half_open_connections);
    RUN_TEST(tcp_rejects_bad_segments);
    RUN_TEST(tcp_reassembly_list);
    RUN_TEST(tcp_rto_follows_rfc6298);
    RUN_TEST(tcp_congestion_window_follows_rfc5681);
    RUN_TEST(tcp_reassembles_out_of_order_segments);
    RUN_TEST(tcp_sender_keeps_to_the_congestion_window);
    RUN_TEST(tcp_options_are_read);
    RUN_TEST(tcp_window_scaling_is_negotiated);
    RUN_TEST(tcp_rings_grow_past_64k);
}
