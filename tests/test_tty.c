#include "test.h"
#include "tty.h"
#include "string_util.h"

TEST(tty_write_read_test) {
    tty_init();

    const char *msg = "TTY Test Input";
    size_t len = strlen(msg);

    tty_write(msg, len);

    char rx[64];
    size_t read_bytes = tty_read(rx, sizeof(rx) - 1);
    rx[read_bytes] = '\0';

    EXPECT_TRUE(read_bytes == len);
    EXPECT_TRUE(strcmp(rx, msg) == 0);
}

void test_suite_tty(void) {
    kprintf("Suite: Phase 25 Virtual Terminal TTY Device\n");
    RUN_TEST(tty_write_read_test);
}
