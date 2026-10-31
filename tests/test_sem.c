#include "test.h"
#include "sem.h"
#include "mutex.h"
#include "sched.h"
#include "pit.h"

// A semaphore counts, and sem_trywait refuses when it is empty rather than wait
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

// Waits on the gate, then notes that it got through
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
    TEST_WAIT(waiter_task->state == TASK_BLOCKED, 100);
    for (int i = 0; i < 50; i++) sched_yield();
    EXPECT_EQ(waiter_passed, 0);
    EXPECT_EQ(waiter_task->state, TASK_BLOCKED);
    EXPECT_TRUE(waiter_task->wait_channel == &gate);

    sem_post(&gate);
    EXPECT_TRUE(waiter_task->state != TASK_BLOCKED); // Woken by the post itself (and perhaps already running on another CPU)

    TEST_WAIT(waiter_passed, 200);
    EXPECT_EQ(waiter_passed, 1);
    EXPECT_EQ(sem_getvalue(&gate), 0);
}

static sem_t handoff;

// Sleeps five ticks, then posts
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

    uint64_t before = timer_ticks();
    sem_wait(&handoff);
    uint64_t waited = timer_ticks() - before;

    EXPECT_TRUE(waited >= 5);
    EXPECT_TRUE(waited <= 8);
}

// --- Mutex -------------------------------------------------------------------

static mutex_t door = MUTEX_INIT("test-door");
static volatile int inside = 0, most_inside = 0, visits = 0;

// Enters the mutex twenty times, sleeping inside, counting who is inside
static void visitor_entry(void) {
    for (int i = 0; i < 20; i++) {
        mutex_lock(&door);
        int now = ++inside;
        if (now > most_inside) most_inside = now;
        sched_sleep_ticks(1);          // Holding a mutex across a sleep is what it is for
        inside--;
        visits++;
        mutex_unlock(&door);
    }
}

// A mutex can be taken again by its holder, others sleep on it rather than spin,
// and never two are inside at once, although each sleeps there
TEST(mutex_admits_one_holder_and_may_sleep) {
    inside = 0; most_inside = 0; visits = 0;

    EXPECT_TRUE(mutex_trylock(&door));
    EXPECT_TRUE(mutex_held(&door));
    mutex_lock(&door);                 // The holder may take it again...
    mutex_unlock(&door);
    EXPECT_TRUE(mutex_held(&door));    // ...and still holds it after one unlock

    task_t *a = task_create("visitor-a", visitor_entry);
    task_t *b = task_create("visitor-b", visitor_entry);
    task_t *c = task_create("visitor-c", visitor_entry);
    EXPECT_TRUE(a && b && c);

    // While we hold it, nobody gets in: they sleep on it instead of spinning
    TEST_WAIT(a->state == TASK_BLOCKED && b->state == TASK_BLOCKED && c->state == TASK_BLOCKED, 100);
    for (int i = 0; i < 50; i++) sched_yield();
    EXPECT_EQ(visits, 0);
    EXPECT_EQ(a->state, TASK_BLOCKED);
    EXPECT_TRUE(a->wait_channel == &door);

    mutex_unlock(&door);
    EXPECT_TRUE(!mutex_held(&door));

    for (int i = 0; i < 400 && visits < 60; i++) sched_sleep_ticks(1);
    EXPECT_EQ(visits, 60);
    EXPECT_EQ(most_inside, 1);         // Never two at once, although each slept inside
    EXPECT_TRUE(mutex_trylock(&door));
    mutex_unlock(&door);

    while (sched_reap() == 0) sched_yield();
    while (sched_reap() != 0) { }
}

void test_suite_sem(void) {
    kprintf("Suite: Semaphores and Blocking\n");
    RUN_TEST(semaphore_counts);
    RUN_TEST(semaphore_blocks_until_post);
    RUN_TEST(semaphore_wait_with_nothing_runnable);
    RUN_TEST(mutex_admits_one_holder_and_may_sleep);
}
