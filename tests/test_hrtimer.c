#include "test.h"
#include "hrtimer.h"
#include "sched.h"
#include "pit.h"
#include "timer.h"
#include "smp.h"
#include "percpu.h"
#include "tcp.h"

TEST(sleep_lasts_as_long_as_asked) {
    uint64_t before = timer_ticks();
    sched_sleep_ticks(10);
    uint64_t slept = timer_ticks() - before;
    EXPECT_TRUE(slept >= 10);
    EXPECT_TRUE(slept <= 12);

    before = timer_ticks();
    sched_sleep_ticks(0);                          // Returns at once
    EXPECT_TRUE(timer_ticks() - before <= 1);
}

static volatile uint64_t sleeper_woke_at = 0;
static task_t *sleeper_task = NULL;

// Sleeps ten ticks and notes when it woke
static void sleeper_entry(void) {
    sched_sleep_ticks(10);
    sleeper_woke_at = timer_ticks();
}

// A sleeping task is off the run queue: a busy task next to it keeps the CPU,
// and the sleeper still wakes on time.
TEST(sleeping_task_is_blocked_not_spinning) {
    sleeper_woke_at = 0;
    uint64_t started = timer_ticks();
    sleeper_task = task_create("sleeper", sleeper_entry);
    EXPECT_TRUE(sleeper_task != NULL);

    TEST_WAIT(sleeper_task->state == TASK_BLOCKED, 5); // Let it start and go to sleep
    EXPECT_EQ(sleeper_task->state, TASK_BLOCKED);
    EXPECT_TRUE(sleeper_task->wake_tick >= started + 10);

    // Yield as fast as possible: a spinning sleeper would get half of these
    // turns, a blocked one gets none, so the yields return immediately.
    uint64_t yields = 0;
    while (sleeper_woke_at == 0 && timer_ticks() - started < 100) {
        sched_yield();
        yields++;
    }

    EXPECT_TRUE(sleeper_woke_at != 0);
    EXPECT_TRUE(sleeper_woke_at - started >= 10);
    EXPECT_TRUE(sleeper_woke_at - started <= 13);
    EXPECT_TRUE(yields > 1000);
}

// Without a timestamp counter (make test CPU=qemu64,-tsc) the kernel keeps
// its periodic tick and the clock can only count ticks
TEST(clock_without_a_timestamp_counter) {
    EXPECT_EQ(hrtimer_tsc_hz(), 0u);
    EXPECT_TRUE(!timer_is_one_shot());

    uint64_t before = clock_ns(), ticks = timer_ticks();
    sched_sleep_ticks(5);
    uint64_t elapsed = clock_ns() - before;
    EXPECT_TRUE(timer_ticks() - ticks >= 5);
    EXPECT_TRUE(elapsed >= 50000000ULL && elapsed <= 80000000ULL && elapsed % 10000000ULL == 0);

    // A fine sleep is still never short, only coarse
    before = clock_ns();
    EXPECT_TRUE(nanosleep(3000000));
    EXPECT_TRUE(clock_ns() - before >= 3000000);
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

// --- One-shot timers ----------------------------------------------------------

static void spin_ticks(uint64_t ticks) {
    uint64_t until = timer_ticks() + ticks;
    while (timer_ticks() < until) asm volatile("pause");
}

// A CPU with nothing to run is not interrupted by its timer; one that is
// running a task still is, every 10 ms
TEST(idle_cpus_take_no_timer_interrupts) {
    EXPECT_TRUE(timer_is_one_shot());
    test_settle(); // No connection left whose timers would need running
    uint32_t cpus = (uint32_t)smp_get_cpu_total();
    static uint64_t before[MAX_CPUS];

    // Half a second asleep. This task is the only one with anything to do,
    // so every CPU is idle: 50 ticks pass, and the boot CPU's timer fires to
    // end the sleep and for little else.
    int quiet = 0;
    uint64_t taken = 0, elsewhere = 0;
    for (int attempt = 0; attempt < 3 && !quiet; attempt++) {
        for (uint32_t i = 0; i < cpus; i++) before[i] = timer_interrupt_count(i);
        uint64_t ticks = timer_ticks();
        sched_sleep_ticks(50);
        EXPECT_TRUE(timer_ticks() - ticks >= 50);
        taken = timer_interrupt_count(0) - before[0];
        elsewhere = 0;
        for (uint32_t i = 1; i < cpus; i++) elsewhere += timer_interrupt_count(i) - before[i];
        quiet = taken >= 1 && taken <= 5 && elsewhere <= 2;
    }
    if (!quiet) kprintf("    in 50 idle ticks: %lu timer interrupts on the boot CPU, %lu on the others\n", taken, elsewhere);
    EXPECT_EQ(quiet, 1);

    // The same time spent working: one interrupt per tick, give or take
    uint64_t count = timer_interrupt_count(0);
    spin_ticks(30);
    taken = timer_interrupt_count(0) - count;
    EXPECT_TRUE(taken >= 25 && taken <= 40);
}

static volatile uint64_t nap_started, nap_ended;
static volatile uint64_t nap_length;

// Sleeps nap_length nanoseconds and notes when it started and ended
static void napper(void) {
    nap_started = clock_ns();
    nanosleep(nap_length);
    nap_ended = clock_ns();
}

// A fine sleep is slept, not watched: the task is blocked with its deadline
// recorded, and wakes within a fraction of a millisecond of it
TEST(fine_sleeps_block_and_end_on_time) {
    EXPECT_TRUE(timer_is_one_shot());
    uint64_t best = ~0ULL;
    bool blocked = false, never_short = true;
    for (int attempt = 0; attempt < 5; attempt++) {
        nap_started = nap_ended = 0;
        nap_length = 23500000;                     // 23.5 ms: between ticks
        task_t *task = task_create("napper", napper);
        EXPECT_TRUE(task != NULL);
        TEST_WAIT(nap_started != 0, 20);
        sched_sleep_ticks(1);
        uint64_t flags = sched_lock_irqsave();
        if (nap_ended == 0 && task->state == TASK_BLOCKED && task->wake_ns != 0) blocked = true;
        sched_unlock_irqrestore(flags);
        TEST_WAIT(nap_ended != 0, 50);
        uint64_t elapsed = nap_ended - nap_started;
        if (elapsed < nap_length) never_short = false;
        if (elapsed < best) best = elapsed;
        while (sched_reap() == 0) sched_yield();
    }
    EXPECT_TRUE(blocked);
    EXPECT_TRUE(never_short);
    if (best >= 24500000) kprintf("    best of five 23.5 ms sleeps: %lu ns\n", best);
    EXPECT_TRUE(best < 24500000);                  // Not rounded up to 30 ms
}

static volatile uint64_t woke_at[4];
static volatile int woke_order[4], woke_count;

// One of four sleepers, each with an earlier deadline than the last started
static void ordered_sleeper(void) {
    // Which sleeper this is comes from the order they start in
    static volatile int next_index;
    int index = __atomic_fetch_add(&next_index, 1, __ATOMIC_SEQ_CST) % 4;
    // Longest first: each new deadline is earlier than the one the timer is set for
    nanosleep((uint64_t)(4 - index) * 7000000);
    woke_at[index] = clock_ns();
    woke_order[__atomic_fetch_add(&woke_count, 1, __ATOMIC_SEQ_CST) % 4] = index;
}

// Several sleepers with different deadlines, started longest first: the
// timer has to be moved forward for each, and then on from one to the next
TEST(sleepers_wake_in_deadline_order) {
    woke_count = 0;
    for (int i = 0; i < 4; i++) woke_at[i] = 0;
    uint64_t start = clock_ns();
    for (int i = 0; i < 4; i++) EXPECT_TRUE(task_create("sleeper", ordered_sleeper) != NULL);
    TEST_WAIT(woke_count == 4, 100);
    EXPECT_EQ(woke_count, 4);
    // The fourth started sleeps 7 ms, the first 28
    EXPECT_TRUE(woke_order[0] == 3 && woke_order[1] == 2 && woke_order[2] == 1 && woke_order[3] == 0);
    for (int i = 0; i < 4; i++) EXPECT_TRUE(woke_at[i] - start >= (uint64_t)(4 - i) * 7000000);
    size_t reaped = 0;
    TEST_WAIT((reaped += sched_reap()) >= 4, 100);
}

static volatile int reaper_woke;

// Waits, as the kernel's reaper does, for a task to die
static void reaper_waiter(void) {
    sched_wait_for_reapable();
    reaper_woke = 1;
}

static void dies_at_once(void) {}

// What the kernel's own reaper sleeps in: nothing wakes it but a task
// dying with nobody to wait for it
TEST(reaper_sleeps_until_a_task_dies) {
    sched_reap();
    reaper_woke = 0;
    task_t *waiter = task_create("waiter", reaper_waiter);
    EXPECT_TRUE(waiter != NULL);
    TEST_WAIT(waiter->state == TASK_BLOCKED, 20);
    sched_sleep_ticks(10);
    EXPECT_EQ(reaper_woke, 0);
    EXPECT_TRUE(waiter->wake_tick == 0 && waiter->wake_ns == 0); // No deadline: it would sleep for ever

    EXPECT_TRUE(task_create("mayfly", dies_at_once) != NULL);
    TEST_WAIT(reaper_woke, 20);
    EXPECT_EQ(reaper_woke, 1);
    size_t reaped = 0;
    TEST_WAIT((reaped += sched_reap()) >= 2, 100);
    EXPECT_EQ(reaped, (size_t)2);
}

void test_suite_hrtimer(void) {
    kprintf("Suite: Sleeping and Timers\n");
    RUN_TEST(sleep_lasts_as_long_as_asked);
    RUN_TEST(sleeping_task_is_blocked_not_spinning);
    RUN_TEST(reaper_sleeps_until_a_task_dies);
    if (hrtimer_tsc_hz() == 0) {
        RUN_TEST(clock_without_a_timestamp_counter);
        return;
    }
    RUN_TEST(clock_is_calibrated_and_monotonic);
    RUN_TEST(nanosleep_is_finer_than_a_tick);
    RUN_TEST(idle_cpus_take_no_timer_interrupts);
    RUN_TEST(fine_sleeps_block_and_end_on_time);
    RUN_TEST(sleepers_wake_in_deadline_order);
}
