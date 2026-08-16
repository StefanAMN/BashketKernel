#include "test.h"
#include "rtc.h"

TEST(rtc_time_read_test) {
    rtc_init();

    struct rtc_time t;
    rtc_read_time(&t);

    EXPECT_TRUE(t.year >= 2020);
    EXPECT_TRUE(t.month >= 1 && t.month <= 12);
    EXPECT_TRUE(t.day >= 1 && t.day <= 31);
}

void test_suite_rtc(void) {
    kprintf("Suite: Phase 17 Real-Time Clock & CMOS Driver\n");
    RUN_TEST(rtc_time_read_test);
}
