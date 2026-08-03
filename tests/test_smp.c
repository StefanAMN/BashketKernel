#include "test.h"
#include "spinlock.h"
#include "smp.h"
#include "sched.h"

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

// Each CPU reaches its own structure through GS, has its own task state
// segment, and took a breakpoint exception by itself while starting.
TEST(per_cpu_data_and_exceptions) {
    // This CPU: GS points at its structure, which points at itself
    cpu_t *self = this_cpu();
    EXPECT_TRUE(self == cpu_get(0));
    EXPECT_TRUE(self->self == self);
    EXPECT_TRUE(cpu_read_gs_base() == (uint64_t)self);
    EXPECT_TRUE(self->current == sched_get_current_task());
    EXPECT_TRUE(self->tss.ist1 != 0);

    // A breakpoint here is handled and returns
    uint64_t before = self->breakpoints;
    asm volatile("int3");
    EXPECT_TRUE(self->breakpoints == before + 1);
    EXPECT_TRUE(self->breakpoint_lapic_id == self->lapic_id);

    // Every other CPU did the same on its own while starting, and the
    // hardware id it saw in the handler is its own
    for (uint32_t i = 1; i < smp_get_cpu_total(); i++) {
        cpu_t *cpu = cpu_get(i);
        EXPECT_TRUE(cpu->self == cpu && cpu->cpu_id == i);
        EXPECT_TRUE(cpu->breakpoints == 1);
        EXPECT_TRUE(cpu->breakpoint_lapic_id == cpu->lapic_id);
        EXPECT_TRUE(cpu->tss.ist1 != 0 && cpu->tss.ist1 != self->tss.ist1);   // A fault stack of its own
    }
    EXPECT_TRUE(cpu_get(MAX_CPUS) == NULL);
}

void test_suite_smp(void) {
    kprintf("Suite: CPUs and Spinlocks\n");
    RUN_TEST(spinlock_basic_test);
    RUN_TEST(smp_cpus_report_in);
    RUN_TEST(per_cpu_data_and_exceptions);
}
