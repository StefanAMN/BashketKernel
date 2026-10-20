/**
 * @file test_poll.c
 * @brief poll(): readiness of each kind of open file, and the wait
 */

#include "test.h"
#include "poll.h"
#include "file.h"
#include "pipe.h"
#include "tty.h"
#include "net.h"
#include "tcp.h"
#include "mouse.h"
#include "sched.h"
#include "hrtimer.h"
#include "pit.h"
#include "kstring.h"
#include "kerrno.h"

// A descriptor table of the test's own: index 0 and 1 hold the files given
static file_t *table[TASK_MAX_FDS];

// poll the first `count` slots of `table` for `events`, with `timeout_ms`
static int64_t poll_slots(struct pollfd *fds, int count, int events, int64_t timeout_ms) {
    for (int i = 0; i < count; i++) fds[i] = (struct pollfd){ .fd = i, .events = (int16_t)events };
    return poll_wait(table, fds, (size_t)count, timeout_ms);
}

// A pipe's ends: readable once written to, and at the end once the writer
// is gone; writable until the reader is gone
TEST(poll_on_pipes) {
    file_t *reader = NULL, *writer = NULL;
    EXPECT_EQ(file_open_pipe(&reader, &writer), 0);
    table[0] = reader;
    table[1] = writer;
    struct pollfd fds[2];

    EXPECT_EQ(poll_slots(fds, 2, POLLIN | POLLOUT, 0), 1);
    EXPECT_TRUE(fds[0].revents == 0 && fds[1].revents == POLLOUT);

    EXPECT_EQ(file_write(writer, "x", 1), 1);
    EXPECT_EQ(poll_slots(fds, 2, POLLIN | POLLOUT, 0), 2);
    EXPECT_TRUE(fds[0].revents == POLLIN);

    file_close(writer);
    table[1] = NULL;
    char c;
    EXPECT_EQ(file_read(reader, &c, 1), 1);
    EXPECT_EQ(poll_slots(fds, 1, POLLIN, 0), 1);
    EXPECT_TRUE(fds[0].revents == (POLLIN | POLLHUP));             // End of file: a read returns at once

    // The other way round: the write end once the reader has gone
    file_close(reader);
    EXPECT_EQ(file_open_pipe(&reader, &writer), 0);
    table[0] = writer;
    file_close(reader);
    EXPECT_EQ(poll_slots(fds, 1, POLLOUT, 0), 1);
    EXPECT_TRUE(fds[0].revents == POLLERR);                        // Reported though not asked for
    file_close(writer);
    table[0] = NULL;
}

// Descriptors that are not open, and entries to be ignored
TEST(poll_reports_bad_descriptors) {
    struct pollfd fds[3] = { { 5, POLLIN, 0 }, { -1, POLLIN, 0 }, { TASK_MAX_FDS + 3, POLLIN, 0 } };
    EXPECT_EQ(poll_wait(table, fds, 3, 0), 2);
    EXPECT_TRUE(fds[0].revents == POLLNVAL && fds[1].revents == 0 && fds[2].revents == POLLNVAL);
}

static file_t *late_writer;

// Writes one byte after 50 ms
static void write_later(void) {
    sched_sleep_ticks(5);
    file_write(late_writer, "!", 1);
}

// A poller sleeps until the event, and a time limit with no event ends it
TEST(poll_sleeps_until_ready_or_timeout) {
    file_t *reader = NULL;
    EXPECT_EQ(file_open_pipe(&reader, &late_writer), 0);
    table[0] = reader;
    struct pollfd fds[1];

    uint64_t start = clock_ns();
    EXPECT_EQ(poll_slots(fds, 1, POLLIN, 100), 0);                   // Nothing comes: 100 ms, then 0
    uint64_t waited = clock_ns() - start;
    EXPECT_TRUE(waited >= 90000000);

    EXPECT_TRUE(task_create("late-writer", write_later) != NULL);
    start = clock_ns();
    EXPECT_EQ(poll_slots(fds, 1, POLLIN, 5000), 1);                  // Woken by the write, long before 5 s
    waited = clock_ns() - start;
    EXPECT_TRUE(fds[0].revents == POLLIN && waited < 2000000000ULL);

    file_close(reader);
    file_close(late_writer);
    table[0] = NULL;
    TEST_WAIT(sched_reap() > 0, 100);
}

// The terminal in line mode: readable once a whole line has been typed
TEST(poll_on_the_terminal) {
    tty_init();
    EXPECT_TRUE(tty_poll() == POLLOUT);
    tty_input('h');
    tty_input('i');
    EXPECT_TRUE(tty_poll() == POLLOUT);                                // Half a line is not something to read
    tty_input('\n');
    EXPECT_TRUE(tty_poll() == (POLLIN | POLLOUT));
    tty_init();
}

// A UDP socket: readable when a datagram is queued
TEST(poll_on_udp) {
    file_t *sock = file_open_socket();
    EXPECT_TRUE(sock != NULL);
    if (!sock) return;
    EXPECT_EQ(udp_bind(sock->socket, 7300), 0);
    table[0] = sock;
    struct pollfd fds[1];
    EXPECT_EQ(poll_slots(fds, 1, POLLIN | POLLOUT, 0), 1);
    EXPECT_TRUE(fds[0].revents == POLLOUT);
    EXPECT_EQ(udp_sendto(sock->socket, NET_LOCAL_IP, 7300, "hello", 5), 5);  // To ourselves
    EXPECT_EQ(poll_slots(fds, 1, POLLIN, 1000), 1);
    EXPECT_TRUE(fds[0].revents == POLLIN);
    file_close(sock);
    table[0] = NULL;
}

// TCP: a listener is readable when a connection waits to be accepted; a
// connection is writable, and readable when data arrives
TEST(poll_on_tcp) {
    tcp_socket_t *listener = tcp_open();
    EXPECT_TRUE(listener != NULL);
    if (!listener) return;
    EXPECT_EQ(tcp_bind(listener, 7301), 0);
    EXPECT_EQ(tcp_listen(listener, 2), 0);
    EXPECT_TRUE(tcp_poll(listener) == 0);

    tcp_socket_t *client = tcp_open();
    EXPECT_EQ(tcp_connect(client, NET_LOCAL_IP, 7301), 0);
    TEST_WAIT(tcp_poll(listener) == POLLIN, 200);
    EXPECT_TRUE(tcp_poll(listener) == POLLIN);
    int error = 0;
    tcp_socket_t *server = tcp_accept(listener, &error);
    EXPECT_TRUE(server != NULL);
    EXPECT_TRUE(tcp_poll(client) == POLLOUT);

    if (server) {
        EXPECT_EQ(tcp_write(server, "ping", 4), 4);
        TEST_WAIT(tcp_poll(client) & POLLIN, 200);
        EXPECT_TRUE(tcp_poll(client) == (POLLIN | POLLOUT));
        tcp_close(server);
    }
    tcp_close(client);
    tcp_close(listener);
}

// The mouse: readable once it has moved
TEST(poll_on_the_mouse) {
    mouse_flush();
    EXPECT_TRUE(mouse_poll() == 0);
    mouse_handle_byte(0x08);                                          // A packet: no buttons, moved 5 right
    mouse_handle_byte(5);
    mouse_handle_byte(0);
    EXPECT_TRUE(mouse_poll() == POLLIN);
    mouse_flush();
}

void test_suite_poll(void) {
    kprintf("Suite: poll\n");
    RUN_TEST(poll_on_pipes);
    RUN_TEST(poll_reports_bad_descriptors);
    RUN_TEST(poll_sleeps_until_ready_or_timeout);
    RUN_TEST(poll_on_the_terminal);
    RUN_TEST(poll_on_udp);
    RUN_TEST(poll_on_tcp);
    RUN_TEST(poll_on_the_mouse);
}
