#include "test.h"
#include "pipe.h"
#include "string_util.h"

TEST(pipe_write_read_test) {
    pipe_t *p = pipe_create();
    EXPECT_TRUE(p != NULL);

    const char *msg = "Hello Kernel IPC Pipe!";
    size_t len = strlen(msg);

    size_t written = pipe_write(p, msg, len);
    EXPECT_TRUE(written == len);

    char rx[64];
    size_t read_bytes = pipe_read(p, rx, sizeof(rx) - 1);
    rx[read_bytes] = '\0';

    EXPECT_TRUE(read_bytes == len);
    EXPECT_TRUE(strcmp(rx, msg) == 0);

    pipe_destroy(p);
}

void test_suite_pipe(void) {
    kprintf("Suite: Phase 19 IPC Ring-Buffer Pipes\n");
    RUN_TEST(pipe_write_read_test);
}
