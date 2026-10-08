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

    TEST_WAIT(sleeper_task->state == TASK_BLOCKED, 5); // Let it start and go to sleep
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

TEST(clock_is_calibrated_and_monotonic) {
    // The TSC of anything QEMU emulates runs between 100 MHz and 10 GHz
    EXPECT_TRUE(hrtimer_tsc_hz() >= 100000000ULL && hrtimer_tsc_hz() <= 10000000000ULL);

    // Never backwards, and fine-grained: many readings within one tick differ
    uint64_t previous = clock_ns();
    int went_back = 0, distinct = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t now = clock_ns();
        if (now < previous) went_back = 1;
        if (now != previous) distinct++;
        previous = now;
    }
    EXPECT_EQ(went_back, 0);
    EXPECT_TRUE(distinct > 100);

    // It agrees with the timer: 20 ticks are 200 ms. In a virtual machine
    // neither clock is exact against the other for one measurement: after
    // the host stalls the machine, real time has jumped ahead and the
    // emulated timer then delivers its missed ticks in a burst. So one
    // measurement in the expected window out of several is the evidence.
    int in_window = 0;
    for (int attempt = 0; attempt < 5 && !in_window; attempt++) {
        uint64_t start_ns = clock_ns();
        sched_sleep_ticks(20);
        uint64_t elapsed = clock_ns() - start_ns;
        in_window = (elapsed >= 190ULL * 1000000 && elapsed <= 225ULL * 1000000);
    }
    EXPECT_EQ(in_window, 1);
}

// Shortest of several sleeps of `ns`, each checked to be no shorter than asked
static uint64_t best_nanosleep(uint64_t ns, int attempts, int *all_long_enough) {
    uint64_t best = ~0ULL;
    for (int i = 0; i < attempts; i++) {
        uint64_t start = clock_ns();
        if (!nanosleep(ns)) *all_long_enough = 0;
        uint64_t elapsed = clock_ns() - start;
        if (elapsed < ns) *all_long_enough = 0;
        if (elapsed < best) best = elapsed;
    }
    return best;
}

TEST(nanosleep_is_finer_than_a_tick) {
    int never_short = 1;

    // 3 ms: a tick-based sleep could only manage 10 or 20
    EXPECT_TRUE(best_nanosleep(3000000, 5, &never_short) < 6000000);

    // 25 ms: two whole ticks slept, the rest watched
    EXPECT_TRUE(best_nanosleep(25000000, 3, &never_short) < 29000000);
    EXPECT_EQ(never_short, 1);

    uint64_t start = clock_ns();
    EXPECT_TRUE(nanosleep(0));
    EXPECT_TRUE(clock_ns() - start < 5000000);
}

void test_suite_hrtimer(void) {
    kprintf("Suite: Sleeping and Timers\n");
    RUN_TEST(sleep_lasts_as_long_as_asked);
    RUN_TEST(sleeping_task_is_blocked_not_spinning);
    RUN_TEST(clock_is_calibrated_and_monotonic);
    RUN_TEST(nanosleep_is_finer_than_a_tick);
}
