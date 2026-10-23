#include "test.h"
#include "file.h"
#include "localsock.h"
#include "heap.h"
#include "poll.h"
#include "kerrno.h"
#include "kstring.h"

// What goes into one end comes out of the other, both ways; a shutdown ends
// one direction only; closing frees everything
TEST(local_pair_carries_bytes_both_ways) {
    test_settle();
    size_t heap_before = kheap_get_allocated_bytes();
    file_t *a = NULL, *b = NULL;
    char buf[8];
    EXPECT_EQ(file_open_local_pair(&a, &b), 0);
    if (!a || !b) return;

    EXPECT_EQ(file_poll(a) & POLLIN, 0);
    EXPECT_EQ(file_write(a, "ping", 4), 4);
    EXPECT_TRUE(file_poll(b) & POLLIN);
    EXPECT_EQ(file_read(b, buf, sizeof(buf)), 4);
    EXPECT_EQ(memcmp(buf, "ping", 4), 0);
    EXPECT_EQ(file_write(b, "pong!", 5), 5);
    EXPECT_EQ(file_read(a, buf, sizeof(buf)), 5);
    EXPECT_EQ(memcmp(buf, "pong!", 5), 0);

    EXPECT_EQ(file_write(a, "last", 4), 4);
    EXPECT_EQ(file_local_shutdown(a), 0);
    EXPECT_EQ(file_local_shutdown(a), 0);                // Twice is harmless
    EXPECT_EQ(file_write(a, "x", 1), -EPIPE);
    EXPECT_EQ(file_read(b, buf, sizeof(buf)), 4);        // What was sent still arrives
    EXPECT_EQ(file_read(b, buf, sizeof(buf)), 0);        // Then end-of-file
    EXPECT_EQ(file_write(b, "back", 4), 4);              // The other way is still open
    EXPECT_EQ(file_read(a, buf, sizeof(buf)), 4);

    file_close(a);
    EXPECT_EQ(file_write(b, "x", 1), -EPIPE);            // Nobody left to read
    file_close(b);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

// A connection queues until accepted, and works from before that; the queue
// holds what was asked; a closed listener refuses, and what it never accepted
// sees end-of-file
TEST(local_listener_queues_and_refuses) {
    test_settle();
    size_t heap_before = kheap_get_allocated_bytes();
    uint64_t id = 0;
    local_listener_t *listener = local_listener_create(2, &id);
    EXPECT_TRUE(listener != NULL && id != 0);
    if (!listener) return;

    file_t *client = NULL, *unaccepted = NULL, *server = NULL;
    char buf[8];
    EXPECT_EQ(local_listener_poll(listener), 0);
    EXPECT_EQ(local_connect(id, &client), 0);
    EXPECT_EQ(local_connect(id, &unaccepted), 0);
    EXPECT_TRUE(local_listener_poll(listener) & POLLIN);
    EXPECT_EQ(file_write(client, "early", 5), 5);        // Before anyone has accepted

    EXPECT_EQ(local_accept(listener, &server), 0);
    EXPECT_EQ(file_read(server, buf, sizeof(buf)), 5);   // The first in is the first out
    EXPECT_EQ(memcmp(buf, "early", 5), 0);
    EXPECT_EQ(file_write(server, "ok", 2), 2);
    EXPECT_EQ(file_read(client, buf, sizeof(buf)), 2);

    local_listener_close(listener);
    EXPECT_EQ(file_read(unaccepted, buf, sizeof(buf)), 0);
    file_t *late = NULL;
    EXPECT_EQ(local_connect(id, &late), -ECONNREFUSED);
    EXPECT_EQ(file_write(client, "on", 2), 2);           // An accepted connection outlives its listener
    EXPECT_EQ(file_read(server, buf, sizeof(buf)), 2);

    file_close(client);
    file_close(unaccepted);
    file_close(server);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

// The table has a size; a full one refuses cleanly, and numbers are not reused
TEST(local_listener_table_is_bounded) {
    local_listener_t *all[LOCAL_LISTENERS_MAX];
    uint64_t ids[LOCAL_LISTENERS_MAX];
    size_t made = 0;
    while (made < LOCAL_LISTENERS_MAX) {
        all[made] = local_listener_create(1, &ids[made]);
        if (!all[made]) break;
        made++;
    }
    EXPECT_TRUE(made == LOCAL_LISTENERS_MAX);
    uint64_t another = 0;
    EXPECT_TRUE(local_listener_create(1, &another) == NULL);
    uint64_t reused = ids[0];
    for (size_t i = 0; i < made; i++) local_listener_close(all[i]);

    local_listener_t *next = local_listener_create(1, &another);
    EXPECT_TRUE(next != NULL && another != reused);
    if (next) local_listener_close(next);
    file_t *none = NULL;
    EXPECT_EQ(local_connect(reused, &none), -ECONNREFUSED);
}

// Run the local socket tests
void test_suite_localsock(void) {
    kprintf("Suite: Local sockets\n");
    RUN_TEST(local_pair_carries_bytes_both_ways);
    RUN_TEST(local_listener_queues_and_refuses);
    RUN_TEST(local_listener_table_is_bounded);
}
