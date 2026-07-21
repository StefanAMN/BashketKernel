#include "test.h"
#include "pipe.h"
#include "sched.h"
#include "heap.h"
#include "kerrno.h"
#include "string_util.h"

TEST(pipe_passes_bytes_in_order) {
    pipe_t *p = pipe_create();
    EXPECT_TRUE(p != NULL);

    const char *msg = "Hello Kernel IPC Pipe!";
    size_t len = strlen(msg);
    EXPECT_EQ(pipe_write(p, msg, len), (int64_t)len);

    // A short read takes only what was asked for; the rest stays queued
    char rx[64] = {0};
    EXPECT_EQ(pipe_read(p, rx, 5), 5);
    EXPECT_EQ(pipe_read(p, rx + 5, sizeof(rx) - 6), (int64_t)len - 5);
    EXPECT_EQ(strcmp(rx, msg), 0);

    pipe_close_writer(p);
    pipe_close_reader(p);
}

TEST(pipe_end_of_file_and_broken_pipe) {
    size_t heap_before = kheap_get_allocated_bytes();
    char byte = 'x';

    // Writer gone: buffered data is still readable, then reads return 0
    pipe_t *p = pipe_create();
    EXPECT_EQ(pipe_write(p, "ab", 2), 2);
    pipe_close_writer(p);
    char rx[4];
    EXPECT_EQ(pipe_read(p, rx, sizeof(rx)), 2);
    EXPECT_EQ(pipe_read(p, rx, sizeof(rx)), 0);   // End of file, and no hang
    pipe_close_reader(p);

    // Reader gone: writing is an error rather than a wait that never ends
    p = pipe_create();
    pipe_close_reader(p);
    EXPECT_EQ(pipe_write(p, &byte, 1), -EPIPE);
    pipe_close_writer(p);

    // Closing both ends freed both pipes
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

#define BIG_TRANSFER (3 * PIPE_BUFFER_SIZE + 123)

static pipe_t *shared_pipe;
static volatile int64_t writer_result;
static volatile int writer_finished;
static task_t *writer_task;

static void big_writer_entry(void) {
    uint8_t *data = kmalloc(BIG_TRANSFER);
    for (size_t i = 0; i < BIG_TRANSFER; i++) data[i] = (uint8_t)(i * 7);

    // More than the pipe holds: this must sleep until the reader makes room
    writer_result = pipe_write(shared_pipe, data, BIG_TRANSFER);
    kfree(data);
    pipe_close_writer(shared_pipe);
    writer_finished = 1;
}

TEST(pipe_blocks_writer_when_full_and_reader_when_empty) {
    shared_pipe = pipe_create();
    writer_result = 0;
    writer_finished = 0;
    writer_task = task_create("pipe_writer", big_writer_entry);
    EXPECT_TRUE(writer_task != NULL);

    // Let the writer fill the pipe. It can not finish, so it must be asleep.
    for (int i = 0; i < 50; i++) sched_yield();
    EXPECT_EQ(writer_finished, 0);
    EXPECT_EQ(writer_task->state, TASK_BLOCKED);
    EXPECT_EQ(shared_pipe->count, (size_t)PIPE_BUFFER_SIZE);

    // Drain it. Each read wakes the writer; reads on an empty pipe sleep
    // until the writer has produced more. Ends at end-of-file.
    size_t total = 0;
    int in_order = 1;
    uint8_t chunk[300];
    int64_t n;
    while ((n = pipe_read(shared_pipe, chunk, sizeof(chunk))) > 0) {
        for (int64_t i = 0; i < n; i++) {
            if (chunk[i] != (uint8_t)((total + (size_t)i) * 7)) in_order = 0;
        }
        total += (size_t)n;
    }

    EXPECT_EQ(n, 0);
    EXPECT_EQ(total, (size_t)BIG_TRANSFER);
    EXPECT_EQ(in_order, 1);
    EXPECT_EQ(writer_result, (int64_t)BIG_TRANSFER);
    pipe_close_reader(shared_pipe);
}

void test_suite_pipe(void) {
    kprintf("Suite: Pipes\n");
    RUN_TEST(pipe_passes_bytes_in_order);
    RUN_TEST(pipe_end_of_file_and_broken_pipe);
    RUN_TEST(pipe_blocks_writer_when_full_and_reader_when_empty);
}
