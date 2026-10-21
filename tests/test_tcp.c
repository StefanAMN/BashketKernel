#include "test.h"
#include "tcp.h"
#include "net.h"
#include "pit.h"
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

    tcp_socket_t *client = tcp_open();
    EXPECT_TRUE(client != NULL);
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, port), 0);

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
    EXPECT_EQ(sink_done, 1);
    EXPECT_TRUE(sink_bytes == TRANSFER_BYTES);
    EXPECT_TRUE(sink_errors == 0);
    tcp_close(sink_listener);
    const tcp_stats_t *st = tcp_stats();
    kprintf("    %lu segments out, %lu in, %lu retransmissions, %lu fast, %lu out of order (%lu kept), %lu round trips, %lu resets\n",
            st->segments_out, st->segments_in, st->retransmissions, st->fast_retransmissions, st->out_of_order,
            st->out_of_order_kept, st->rtt_samples, st->resets_sent);
}

TEST(tcp_large_transfer_arrives_intact) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t samples_before = tcp_stats()->rtt_samples;
    transfer(7103);
    EXPECT_TRUE(tcp_stats()->rtt_samples > samples_before + 10);  // Round trips are measured
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

    tcp_test_drop_every(7);
    transfer(7104);
    wait_for_sockets_to_go(sockets_before);
    tcp_test_drop_every(0);

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
    RUN_TEST(tcp_rejects_bad_segments);
    RUN_TEST(tcp_reassembly_list);
    RUN_TEST(tcp_rto_follows_rfc6298);
    RUN_TEST(tcp_reassembles_out_of_order_segments);
}
