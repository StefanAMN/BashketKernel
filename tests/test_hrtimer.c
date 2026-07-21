#include "test.h"
#include "hrtimer.h"
#include "sched.h"
#include "pit.h"

TEST(sleep_lasts_as_long_as_asked) {
    uint64_t before = pit_get_ticks();
    sched_sleep_ticks(10);
    uint64_t slept = pit_get_ticks() - before;
    EXPECT_TRUE(slept >= 10);
    EXPECT_TRUE(slept <= 12);

    // nanosleep rounds up to whole ticks: 25 ms at 100 Hz is 3 ticks
    before = pit_get_ticks();
    nanosleep(25 * 1000000ULL);
    slept = pit_get_ticks() - before;
    EXPECT_TRUE(slept >= 3);
    EXPECT_TRUE(slept <= 5);

    before = pit_get_ticks();
    sched_sleep_ticks(0);                          // Returns at once
    EXPECT_TRUE(pit_get_ticks() - before <= 1);
}

static volatile uint64_t sleeper_woke_at = 0;
static task_t *sleeper_task = NULL;

static void sleeper_entry(void) {
    sched_sleep_ticks(10);
    sleeper_woke_at = pit_get_ticks();
}

// A sleeping task is off the run queue: a busy task next to it keeps the CPU,
// and the sleeper still wakes on time.
TEST(sleeping_task_is_blocked_not_spinning) {
    sleeper_woke_at = 0;
    uint64_t started = pit_get_ticks();
    sleeper_task = task_create("sleeper", sleeper_entry);
    EXPECT_TRUE(sleeper_task != NULL);

    sched_yield();                                 // Let it start and go to sleep
    EXPECT_EQ(sleeper_task->state, TASK_BLOCKED);
    EXPECT_TRUE(sleeper_task->wake_tick >= started + 10);

    // Yield as fast as possible: a spinning sleeper would get half of these
    // turns, a blocked one gets none, so the yields return immediately.
    uint64_t yields = 0;
    while (sleeper_woke_at == 0 && pit_get_ticks() - started < 100) {
        sched_yield();
        yields++;
    }

    EXPECT_TRUE(sleeper_woke_at != 0);
    EXPECT_TRUE(sleeper_woke_at - started >= 10);
    EXPECT_TRUE(sleeper_woke_at - started <= 13);
    EXPECT_TRUE(yields > 1000);
}

void test_suite_hrtimer(void) {
    kprintf("Suite: Sleeping and Timers\n");
    RUN_TEST(sleep_lasts_as_long_as_asked);
    RUN_TEST(sleeping_task_is_blocked_not_spinning);
}
