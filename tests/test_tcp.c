#include "test.h"
#include "tcp.h"
#include "net.h"
#include "pit.h"
#include "heap.h"
#include "sched.h"
#include "kstring.h"
#include "kerrno.h"

// All of these talk to ourselves: a segment for our own address comes
// straight back in, so both ends of a connection are in this kernel.

// --- A server task that echoes one connection --------------------------------

static tcp_socket_t *echo_listener;
static volatile int echo_served;
static volatile uint64_t echo_bytes;

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

static tcp_socket_t *start_echo_server(uint16_t port) {
    echo_served = 0;
    echo_bytes = 0;
    echo_listener = tcp_open();
    if (!echo_listener) return NULL;
    if (tcp_bind(echo_listener, port) != 0 || tcp_listen(echo_listener, 4) != 0) return NULL;
    task_create("tcp_echo", echo_server_entry);
    return echo_listener;
}

static void wait_for_sockets_to_go(size_t count) {
    // Closing finishes in the background (the last acknowledgements, then
    // two seconds in TIME_WAIT)
    TEST_WAIT(tcp_sockets_in_use() == count, 600);
}

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
    kprintf("    %lu segments out, %lu in, %lu retransmissions, %lu out of order, %lu resets\n",
            st->segments_out, st->segments_in, st->retransmissions, st->out_of_order, st->resets_sent);
}

TEST(tcp_large_transfer_arrives_intact) {
    size_t sockets_before = tcp_sockets_in_use();
    transfer(7103);
    wait_for_sockets_to_go(sockets_before);
    EXPECT_TRUE(tcp_sockets_in_use() == sockets_before);
}

// One segment in seven (data, acknowledgement, SYN or FIN alike) is thrown
// away instead of sent. Everything must still arrive, once, in order.
TEST(tcp_survives_lost_segments) {
    size_t sockets_before = tcp_sockets_in_use();
    uint64_t retransmissions_before = tcp_stats()->retransmissions;
    uint64_t dropped_before = tcp_stats()->dropped_for_test;

    tcp_test_drop_every(7);
    transfer(7104);
    wait_for_sockets_to_go(sockets_before);
    tcp_test_drop_every(0);

    EXPECT_TRUE(tcp_stats()->dropped_for_test > dropped_before + 20);
    EXPECT_TRUE(tcp_stats()->retransmissions > retransmissions_before);
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
}
