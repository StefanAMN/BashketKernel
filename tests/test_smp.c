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

TEST(smp_cpus_report_in) {
    // However many CPUs the machine has (make test uses one; make test-smp
    // boots with four), every one of them must have started
    EXPECT_TRUE(smp_get_cpu_total() >= 1 && smp_get_cpu_total() <= MAX_CPUS);
    EXPECT_TRUE(smp_get_cpu_count() == smp_get_cpu_total());

    per_cpu_t *bsp = smp_get_per_cpu(0);
    EXPECT_TRUE(bsp != NULL && bsp->active);
    EXPECT_TRUE(smp_get_per_cpu(MAX_CPUS) == NULL);

    // Each started CPU has its own hardware identifier
    for (uint32_t i = 0; i < smp_get_cpu_total(); i++) {
        EXPECT_TRUE(smp_get_per_cpu(i)->active);
        for (uint32_t j = 0; j < i; j++) {
            EXPECT_TRUE(smp_get_per_cpu(i)->lapic_id != smp_get_per_cpu(j)->lapic_id);
        }
    }
}

void test_suite_smp(void) {
    kprintf("Suite: CPUs and Spinlocks\n");
    RUN_TEST(spinlock_basic_test);
    RUN_TEST(smp_cpus_report_in);
}
