#include "test.h"
#include "sched.h"

static volatile int task1_counter = 0;
static volatile int task2_counter = 0;

static void dummy_task_1(void) {
    for (int i = 0; i < 5; i++) {
        task1_counter++;
        sched_yield();
    }
}

static void dummy_task_2(void) {
    for (int i = 0; i < 5; i++) {
        task2_counter++;
        sched_yield();
    }
}

TEST(multitasking_cooperative) {
    task_t *t1 = task_create("dummy1", dummy_task_1);
    task_t *t2 = task_create("dummy2", dummy_task_2);

    EXPECT_TRUE(t1 != NULL);
    EXPECT_TRUE(t2 != NULL);

    // Yield several times to run dummy tasks
    for (int i = 0; i < 15; i++) {
        sched_yield();
    }

    EXPECT_EQ(task1_counter, 5);
    EXPECT_EQ(task2_counter, 5);
}

void test_suite_sched(void) {
    kprintf("Suite: Phase 4 Multitasking & Cooperative Scheduler\n");
    RUN_TEST(multitasking_cooperative);
}
