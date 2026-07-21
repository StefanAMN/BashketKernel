#include "test.h"
#include "sem.h"
#include "sched.h"
#include "pit.h"

TEST(semaphore_counts) {
    sem_t sem;
    sem_init(&sem, 2);
    EXPECT_EQ(sem_getvalue(&sem), 2);

    sem_wait(&sem);
    EXPECT_TRUE(sem_trywait(&sem));
    EXPECT_EQ(sem_getvalue(&sem), 0);
    EXPECT_TRUE(!sem_trywait(&sem));   // Empty: refuses instead of waiting

    sem_post(&sem);
    EXPECT_EQ(sem_getvalue(&sem), 1);
}

static sem_t gate;
static volatile int waiter_passed = 0;
static task_t *waiter_task = NULL;

static void waiter_entry(void) {
    sem_wait(&gate);
    waiter_passed = 1;
}

// A task waiting on an empty semaphore must be BLOCKED (off the CPU), not
// spinning, and must run again once someone posts.
TEST(semaphore_blocks_until_post) {
    sem_init(&gate, 0);
    waiter_passed = 0;
    waiter_task = task_create("sem_waiter", waiter_entry);
    EXPECT_TRUE(waiter_task != NULL);

    // Give it plenty of chances to run; it must stay parked
    for (int i = 0; i < 50; i++) sched_yield();
    EXPECT_EQ(waiter_passed, 0);
    EXPECT_EQ(waiter_task->state, TASK_BLOCKED);
    EXPECT_TRUE(waiter_task->wait_channel == &gate);

    sem_post(&gate);
    EXPECT_EQ(waiter_task->state, TASK_READY);   // Woken by the post itself

    for (int i = 0; i < 1000 && !waiter_passed; i++) sched_yield();
    EXPECT_EQ(waiter_passed, 1);
    EXPECT_EQ(sem_getvalue(&gate), 0);
}

static sem_t handoff;

static void late_poster_entry(void) {
    sched_sleep_ticks(5);
    sem_post(&handoff);
}

// The waiter here is the test itself, and the only other task is asleep, so
// for 5 ticks nothing at all is runnable: the CPU has to idle in hlt and
// still come back when the timer wakes the poster.
TEST(semaphore_wait_with_nothing_runnable) {
    sem_init(&handoff, 0);
    EXPECT_TRUE(task_create("sem_poster", late_poster_entry) != NULL);

    uint64_t before = pit_get_ticks();
    sem_wait(&handoff);
    uint64_t waited = pit_get_ticks() - before;

    EXPECT_TRUE(waited >= 5);
    EXPECT_TRUE(waited <= 8);
}

void test_suite_sem(void) {
    kprintf("Suite: Semaphores and Blocking\n");
    RUN_TEST(semaphore_counts);
    RUN_TEST(semaphore_blocks_until_post);
    RUN_TEST(semaphore_wait_with_nothing_runnable);
}
