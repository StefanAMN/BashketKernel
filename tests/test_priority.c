#include "test.h"
#include "priority.h"
#include "sched.h"

TEST(task_priority_test) {
    task_t *current = sched_get_current_task();
    EXPECT_TRUE(current != NULL);

    task_set_priority(current, PRIO_HIGH);
    EXPECT_TRUE(task_get_priority(current) == PRIO_HIGH);

    task_set_priority(current, PRIO_NORMAL);
    EXPECT_TRUE(task_get_priority(current) == PRIO_NORMAL);
}

void test_suite_priority(void) {
    kprintf("Suite: Phase 22 Priority-Based Scheduler\n");
    RUN_TEST(task_priority_test);
}
