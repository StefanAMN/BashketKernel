#include "test.h"
#include "process.h"

TEST(process_fork_test) {
    pid_t child = process_fork();
    EXPECT_TRUE(child >= 100);
}

TEST(process_execve_wait_test) {
    EXPECT_EQ(process_execve("/bin/sh"), 0);

    int status = -1;
    pid_t waited = process_waitpid(100, &status);
    EXPECT_EQ(waited, 100);
    EXPECT_EQ(status, 0);

    EXPECT_EQ(process_kill(100, SIGKILL), 0);
}

void test_suite_process(void) {
    kprintf("Suite: Phase 12 UNIX Process Lifecycle & Signals\n");
    RUN_TEST(process_fork_test);
    RUN_TEST(process_execve_wait_test);
}
