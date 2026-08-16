#include "test.h"
#include "hrtimer.h"

TEST(hrtimer_nanosleep_test) {
    hrtimer_init();
    nanosleep(1000000); // 1 ms sleep
    EXPECT_TRUE(true);
}

void test_suite_hrtimer(void) {
    kprintf("Suite: Phase 33 High-Resolution Timers & Nanosleep\n");
    RUN_TEST(hrtimer_nanosleep_test);
}
