#include "test.h"
#include "spinlock.h"
#include "smp.h"

static spinlock_t test_lock = SPINLOCK_INIT;
static volatile int counter = 0;

TEST(spinlock_basic_test) {
    spinlock_acquire(&test_lock);
    counter++;
    EXPECT_EQ(counter, 1);
    spinlock_release(&test_lock);
}

TEST(smp_bsp_cpu_test) {
    EXPECT_TRUE(smp_get_cpu_count() >= 1);
    per_cpu_t *bsp = smp_get_per_cpu(0);
    EXPECT_TRUE(bsp != NULL);
    EXPECT_TRUE(bsp->active == true);
}

void test_suite_smp(void) {
    kprintf("Suite: Phase 10 Symmetric Multiprocessing (SMP & Spinlocks)\n");
    RUN_TEST(spinlock_basic_test);
    RUN_TEST(smp_bsp_cpu_test);
}
