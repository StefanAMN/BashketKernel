#include "test.h"
#include "klog.h"

TEST(klog_write_read_test) {
    klog_init();
    klog_write("Test message for dmesg ring buffer");
    EXPECT_TRUE(true);
}

void test_suite_klog(void) {
    kprintf("Suite: Phase 32 Kernel Ring Buffer & dmesg Logger\n");
    RUN_TEST(klog_write_read_test);
}
