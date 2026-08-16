#include "test.h"
#include "sem.h"

TEST(semaphore_sync_test) {
    sem_t sem;
    sem_init(&sem, 1);

    EXPECT_TRUE(sem_getvalue(&sem) == 1);

    sem_wait(&sem);
    EXPECT_TRUE(sem_getvalue(&sem) == 0);

    sem_post(&sem);
    EXPECT_TRUE(sem_getvalue(&sem) == 1);
}

void test_suite_sem(void) {
    kprintf("Suite: Phase 23 Counting Semaphores\n");
    RUN_TEST(semaphore_sync_test);
}
