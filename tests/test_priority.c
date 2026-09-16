#include "test.h"
#include "priority.h"
#include "sched.h"

TEST(priority_is_stored_and_clamped) {
    task_t *current = sched_get_current_task();
    EXPECT_EQ(task_get_priority(current), PRIO_NORMAL);

    task_set_priority(current, PRIO_HIGH);
    EXPECT_EQ(task_get_priority(current), PRIO_HIGH);
    task_set_priority(current, 200);                 // Out of range: lowest
    EXPECT_EQ(task_get_priority(current), PRIO_LOW);
    task_set_priority(current, PRIO_NORMAL);

    EXPECT_TRUE(priority_time_slice(PRIO_HIGH) > priority_time_slice(PRIO_NORMAL));
    EXPECT_TRUE(priority_time_slice(PRIO_NORMAL) > priority_time_slice(PRIO_LOW));
    EXPECT_TRUE(priority_time_slice(PRIO_LOW) >= 1);
}

static volatile int stop_spinning;
static volatile int spinners_done;

static void spinner_entry(void) {
    while (!stop_spinning) { } // Pure CPU use: only the timer takes it away
    spinners_done++;
}

// Two tasks that both want the CPU all the time. The scheduler should hand
// it out in proportion to their time slices, and starve neither.
TEST(priority_decides_cpu_share) {
    stop_spinning = 0;
    spinners_done = 0;

    task_t *high = task_create("spin_high", spinner_entry);
    task_t *low = task_create("spin_low", spinner_entry);
    EXPECT_TRUE(high != NULL && low != NULL);
    task_set_priority(high, PRIO_HIGH);
    task_set_priority(low, PRIO_LOW);

    uint64_t high_before = high->cpu_ticks, low_before = low->cpu_ticks;
    sched_sleep_ticks(100);                          // This task is asleep: they have the CPU to themselves
    uint64_t high_ticks = high->cpu_ticks - high_before;
    uint64_t low_ticks = low->cpu_ticks - low_before;

    stop_spinning = 1;
    for (int i = 0; i < 1000 && spinners_done < 2; i++) sched_yield();
    EXPECT_EQ(spinners_done, 2);

    // Slices are 4 and 1 ticks, so about 80 and 20 of the 100 ticks
    EXPECT_TRUE(low_ticks >= 10);                    // Not starved
    EXPECT_TRUE(high_ticks + low_ticks >= 90);       // Nearly all ticks went to the two
    EXPECT_TRUE(high_ticks >= 3 * low_ticks);
    EXPECT_TRUE(high_ticks <= 6 * low_ticks);
}

void test_suite_priority(void) {
    kprintf("Suite: Priority Scheduling\n");
    RUN_TEST(priority_is_stored_and_clamped);
    RUN_TEST(priority_decides_cpu_share);
}
