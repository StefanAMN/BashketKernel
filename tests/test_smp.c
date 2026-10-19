#include "test.h"
#include "spinlock.h"
#include "smp.h"
#include "sched.h"
#include "sem.h"
#include "heap.h"
#include "pmm.h"
#include "cow.h"
#include "vmm.h"
#include "hrtimer.h"
#include "random.h"
#include "kstring.h"

static spinlock_t test_lock = SPINLOCK_INIT;
static volatile int counter = 0;

TEST(spinlock_basic_test) {
    uint64_t flags = spinlock_acquire_irqsave(&test_lock);
    counter++;
    EXPECT_EQ(counter, 1);
    EXPECT_TRUE(spinlock_held(&test_lock));
    spinlock_release_irqrestore(&test_lock, flags);
    EXPECT_TRUE(!spinlock_held(&test_lock));
}

TEST(smp_cpus_report_in) {
    // However many CPUs the machine has (make test uses one; make test-smp
    // boots with four), every one of them must have started
    EXPECT_TRUE(smp_get_cpu_total() >= 1 && smp_get_cpu_total() <= MAX_CPUS);
    EXPECT_TRUE(smp_get_cpu_count() == smp_get_cpu_total());

    cpu_t *bsp = cpu_get(0);
    EXPECT_TRUE(bsp != NULL && bsp->active);
    EXPECT_TRUE(cpu_get(MAX_CPUS) == NULL);

    // Each started CPU has its own hardware identifier
    for (uint32_t i = 0; i < smp_get_cpu_total(); i++) {
        EXPECT_TRUE(cpu_get(i)->active);
        for (uint32_t j = 0; j < i; j++) {
            EXPECT_TRUE(cpu_get(i)->lapic_id != cpu_get(j)->lapic_id);
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

// --- The locking rules are checked -------------------------------------------

static spinlock_t outer_lock = SPINLOCK_RANKED("test-outer", LOCK_RANK_FS);
static spinlock_t inner_lock = SPINLOCK_RANKED("test-inner", LOCK_RANK_PMM);

TEST(lock_rules_are_checked) {
    // Everything below happens with interrupts off, so that the violations
    // caused on purpose are forgotten again before the timer could report them
    uint64_t irq = irq_save();
    uint32_t before = lock_debug_violations();
    uint32_t held_before = lock_debug_held_count();

    // In order: nothing to report
    spinlock_acquire(&outer_lock);
    spinlock_acquire(&inner_lock);
    EXPECT_TRUE(lock_debug_held_count() == held_before + 2);
    EXPECT_TRUE(spinlock_held(&outer_lock) && spinlock_held(&inner_lock));
    spinlock_release(&inner_lock);
    spinlock_release(&outer_lock);
    EXPECT_TRUE(lock_debug_violations() == before);

    // Released in the order taken rather than the reverse: allowed
    spinlock_acquire(&outer_lock);
    spinlock_acquire(&inner_lock);
    spinlock_release(&outer_lock);
    spinlock_release(&inner_lock);
    EXPECT_TRUE(lock_debug_violations() == before);
    EXPECT_TRUE(lock_debug_held_count() == held_before);

    // Out of order: reported
    spinlock_acquire(&inner_lock);
    spinlock_acquire(&outer_lock);
    EXPECT_TRUE(lock_debug_violations() == before + 1);
    spinlock_release(&outer_lock);
    spinlock_release(&inner_lock);

    // Released by someone who does not hold it: reported
    spinlock_release(&outer_lock);
    EXPECT_TRUE(lock_debug_violations() == before + 2);

    lock_debug_reset();
    irq_restore(irq);

    // Taken with interrupts enabled: reported
    if (irq & 0x200) {
        asm volatile("cli");
        uint32_t clean = lock_debug_violations();
        asm volatile("sti");
        spinlock_acquire(&test_lock);
        asm volatile("cli");
        spinlock_release(&test_lock);
        EXPECT_TRUE(lock_debug_violations() == clean + 1);
        lock_debug_reset();
        asm volatile("sti");
    }
    EXPECT_TRUE(lock_debug_violations() == 0);   // And nothing in the whole run so far
}

// --- Other CPUs --------------------------------------------------------------

static volatile uint64_t remote_ran_on = 0;

// Run on another CPU by smp_call: notes which CPU, counts the call
static void remote_hello(void *arg) {
    __atomic_or_fetch(&remote_ran_on, 1ULL << this_cpu()->cpu_id, __ATOMIC_SEQ_CST);
    (*(volatile int *)arg)++;
}

TEST(functions_run_on_other_cpus) {
    static volatile int calls = 0;
    calls = 0;
    remote_ran_on = 0;

    EXPECT_TRUE(!smp_call(0, remote_hello, (void *)&calls, true));          // Not on ourselves (this task stays on CPU 0)
    EXPECT_TRUE(!smp_call(MAX_CPUS, remote_hello, (void *)&calls, true));   // Nor on a CPU that is not there

    uint32_t others = smp_get_cpu_total() - 1;
    for (uint32_t i = 1; i <= others; i++) {
        EXPECT_TRUE(smp_call(i, remote_hello, (void *)&calls, true));
        EXPECT_TRUE(!smp_call_busy(i));
    }
    EXPECT_TRUE((uint32_t)calls == others);
    for (uint32_t i = 1; i <= others; i++) EXPECT_TRUE(remote_ran_on & (1ULL << i));
    EXPECT_TRUE(!(remote_ran_on & 1));
}

// Every CPU does the same mix of work on the same structures at the same
// time: the allocators, page tables, a semaphore, wakeups, the clock. If any
// of them were still protected only by "interrupts are off", which keeps no
// other CPU out, counts would go wrong or a list would be corrupted.
#define HAMMER_ROUNDS 3000

static spinlock_t hammer_lock = SPINLOCK_RANKED("test-hammer", LOCK_RANK_FS);
static volatile uint64_t hammer_counter = 0;   // Changed only under hammer_lock, and not atomically
static sem_t hammer_sem;
static volatile uint32_t hammer_errors = 0;
static char hammer_channel;

// Allocates, checks and frees random blocks and takes locks for
// HAMMER_ROUNDS rounds, from a seed of its own (`arg`)
static void hammer(void *arg) {
    uint64_t seed = (uint64_t)arg * 0x9E3779B97F4A7C15ULL + 1;
    uint64_t last_time = 0;

    for (int round = 0; round < HAMMER_ROUNDS; round++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t size = 16 + (size_t)((seed >> 33) % 700);

        // Heap: the block must be ours alone until we free it
        uint8_t *block = kmalloc(size);
        if (block) {
            memset(block, (int)(seed >> 56), size);
            for (size_t i = 0; i < size; i += 61) {
                if (block[i] != (uint8_t)(seed >> 56)) hammer_errors++;
            }
            kfree(block);
        } else {
            hammer_errors++;
        }

        // Frames, and the reference counts used by copy-on-write
        void *frame = pmm_alloc_frame();
        if (frame) {
            uint64_t *words = (uint64_t *)((uint64_t)frame + pmm_get_hhdm_offset());
            words[0] = seed;
            pmm_ref_frame(frame);
            if (pmm_get_frame_ref(frame) != 2 || words[0] != seed) hammer_errors++;
            pmm_unref_frame(frame);
            pmm_free_frame(frame);
        } else {
            hammer_errors++;
        }

        // Page tables: an address space of our own, built and torn down
        if (round % 16 == 0) {
            uint64_t *space = vmm_create_address_space();
            if (space) {
                if (!vmm_alloc_user_page(space, 0x400000 + (seed & 0xFF000), VMM_WRITABLE)) hammer_errors++;
                vmm_destroy_address_space(space);
            } else {
                hammer_errors++;
            }
        }

        // A plain read-modify-write, safe only if the lock really excludes
        uint64_t flags = spinlock_acquire_irqsave(&hammer_lock);
        uint64_t value = hammer_counter;
        asm volatile("pause");
        hammer_counter = value + 1;
        spinlock_release_irqrestore(&hammer_lock, flags);

        // Semaphore and scheduler: what goes up comes down, wakeups find
        // their way through the task list while it is in use elsewhere
        sem_post(&hammer_sem);
        while (!sem_trywait(&hammer_sem)) asm volatile("pause");
        sched_wakeup(&hammer_channel);

        // The clock never goes backwards, whichever CPU reads it
        uint64_t now = clock_ns();
        if (now < last_time) hammer_errors++;
        last_time = now;
        (void)get_random_u32();
    }
}

static volatile int hammer_tasks_done = 0;

// hammer() as a task, counting itself done
static void hammer_task(void) {
    hammer((void *)(uint64_t)(100 + sched_get_current_task()->pid));
    __atomic_add_fetch(&hammer_tasks_done, 1, __ATOMIC_SEQ_CST);
}

TEST(cpus_share_the_kernel_safely) {
    uint32_t others = smp_get_cpu_total() - 1;
    sem_init(&hammer_sem, 0);
    hammer_counter = 0;
    hammer_errors = 0;
    hammer_tasks_done = 0;

    sched_reap();
    size_t frames_before = test_free_frames();
    size_t heap_before = kheap_get_allocated_bytes();

    // The other CPUs start and are left running; this CPU joins in with
    // two tasks that preempt each other, and with this test itself
    for (uint32_t i = 1; i <= others; i++) EXPECT_TRUE(smp_call(i, hammer, (void *)(uint64_t)i, false));
    EXPECT_TRUE(task_create("hammer-a", hammer_task) != NULL);
    EXPECT_TRUE(task_create("hammer-b", hammer_task) != NULL);
    hammer((void *)99);

    while (hammer_tasks_done < 2) sched_yield();
    for (uint32_t i = 1; i <= others; i++) {
        while (smp_call_busy(i)) asm volatile("pause");
    }
    // Both tasks have counted themselves done, which is not yet "exited":
    // wait until both have been freed, not until something has
    size_t reaped = 0;
    TEST_WAIT((reaped += sched_reap()) >= 2, 200);
    EXPECT_EQ(reaped, (size_t)2);

    EXPECT_TRUE(hammer_errors == 0);
    EXPECT_TRUE(hammer_counter == (uint64_t)(others + 3) * HAMMER_ROUNDS);
    EXPECT_EQ(sem_getvalue(&hammer_sem), 0);
    EXPECT_TRUE(test_free_frames() == frames_before);        // Nothing leaked, nothing freed twice
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
    EXPECT_TRUE(lock_debug_violations() == 0);
}

// --- Tasks on every CPU ------------------------------------------------------

static volatile int burners_stop, burners_done;
static volatile uint64_t burner_cpus_seen;      // Bit n: some burner ran on CPU n

// Uses the CPU until told to stop, noting every CPU it found itself on
static void burner_entry(void) {
    while (!burners_stop) {
        uint64_t flags = irq_save();
        uint32_t here = this_cpu()->cpu_id;
        irq_restore(flags);
        __atomic_or_fetch(&burner_cpus_seen, 1ULL << here, __ATOMIC_SEQ_CST);
    }
    __atomic_add_fetch(&burners_done, 1, __ATOMIC_SEQ_CST);
}

// How many CPUs take part in scheduling
static uint32_t online_cpus(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < smp_get_cpu_total(); i++) if (cpu_get(i)->online) n++;
    return n;
}

TEST(tasks_run_on_every_cpu_at_once) {
    uint32_t cpus = online_cpus();
    EXPECT_TRUE(cpus == smp_get_cpu_total());    // Every CPU that started takes part
    EXPECT_TRUE(cpu_get(0)->idle != NULL);

    burners_stop = 0; burners_done = 0; burner_cpus_seen = 0;
    task_t *burners[MAX_CPUS];
    for (uint32_t i = 0; i < cpus; i++) {
        burners[i] = task_create("burner", burner_entry);
        EXPECT_TRUE(burners[i] != NULL);
    }

    // With more runnable tasks than CPUs (the burners and this test), every
    // CPU must be running one at the same moment
    uint32_t most_at_once = 0;
    for (int i = 0; i < 30; i++) {
        sched_sleep_ticks(1);
        uint32_t now = sched_running_count();
        if (now > most_at_once) most_at_once = now;
    }
    EXPECT_TRUE(most_at_once == cpus);

    // ...and between them the burners got onto every CPU
    for (uint32_t i = 0; i < cpus; i++) EXPECT_TRUE(burner_cpus_seen & (1ULL << i));

    // A task tied to one CPU is seen there and nowhere else from then on
    if (cpus > 1) {
        task_set_affinity(burners[0], (int32_t)(cpus - 1));
        sched_sleep_ticks(10);                   // Long enough to be rescheduled onto it
        for (int i = 0; i < 20; i++) {
            sched_sleep_ticks(1);
            uint64_t flags = sched_lock_irqsave();
            bool running = burners[0]->state == TASK_RUNNING;
            uint32_t where = burners[0]->last_cpu;
            sched_unlock_irqrestore(flags);
            if (running) EXPECT_TRUE(where == cpus - 1);
        }
        task_set_affinity(burners[0], 99);       // No such CPU: free to run anywhere
        EXPECT_TRUE(burners[0]->affinity == TASK_ANY_CPU);
    }

    burners_stop = 1;
    TEST_WAIT(burners_done == (int)cpus, 300);
    EXPECT_EQ(burners_done, (int)cpus);
    while (sched_reap() == 0) sched_yield();
    while (sched_reap() != 0) { }

    // This test itself has not left the boot CPU
    uint64_t flags = irq_save();
    EXPECT_TRUE(this_cpu()->cpu_id == 0);
    irq_restore(flags);
}

// Work that can be split is done sooner on more CPUs. The amount of work is
// fixed; the time for it is measured with one worker and with one per CPU.
#define SUM_TO 30000000ULL

static volatile int workers_done;
static volatile uint64_t worker_sums;

// A fixed amount of arithmetic, then counted done
static void worker_entry(void) {
    volatile uint64_t sum = 0;
    for (uint64_t i = 1; i <= SUM_TO; i++) sum += i;
    __atomic_add_fetch(&worker_sums, sum, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&workers_done, 1, __ATOMIC_SEQ_CST);
}

// Nanoseconds for `count` workers started together to finish
static uint64_t time_workers(int count) {
    workers_done = 0;
    uint64_t start = clock_ns();
    for (int i = 0; i < count; i++) task_create("worker", worker_entry);
    while (workers_done < count) sched_sleep_ticks(1);
    uint64_t elapsed = clock_ns() - start;
    while (sched_reap() == 0) sched_yield();
    while (sched_reap() != 0) { }
    return elapsed;
}

TEST(more_cpus_do_more_work) {
    uint32_t cpus = online_cpus();
    int workers = cpus > 4 ? 4 : (int)cpus;

    worker_sums = 0;
    uint64_t one = time_workers(1);
    uint64_t many = time_workers(workers);
    EXPECT_TRUE(worker_sums == (uint64_t)(workers + 1) * (SUM_TO * (SUM_TO + 1) / 2)); // Nobody's sum was disturbed

    kprintf("    1 worker: %lu ms; %d workers on %u CPUs: %lu ms\n", one / 1000000, workers, cpus, many / 1000000);
    if (workers > 1) {
        // Perfect scaling would take as long as one worker; no scaling,
        // `workers` times as long. Half way is asked for: the machine under
        // QEMU may not have that many free cores of its own.
        EXPECT_TRUE(many * 2 < one * (uint64_t)(workers + 1));
    }
}

// --- Stale page-table lookups on other CPUs ----------------------------------

#define PROBE_VA 0xFFFFC90000000000ULL   // An otherwise unused kernel address

static volatile uint64_t probe_value;

// Run on another CPU: read through the probe mapping, caching it there
static void probe_read(void *arg) {
    (void)arg;
    probe_value = *(volatile uint64_t *)PROBE_VA; // Leaves the lookup in this CPU's TLB
}

TEST(other_cpus_forget_removed_kernel_mappings) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    void *first = pmm_alloc_frame();
    void *second = pmm_alloc_frame();
    EXPECT_TRUE(first != NULL && second != NULL);
    *(uint64_t *)((uint64_t)first + hhdm) = 0x1111;
    *(uint64_t *)((uint64_t)second + hhdm) = 0x2222;

    size_t frames_with_tables = 0;
    EXPECT_TRUE(vmm_map_page(NULL, PROBE_VA, (uint64_t)first, VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE));
    frames_with_tables = test_free_frames();

    // Every other CPU reads through the mapping, so each has it cached
    for (uint32_t i = 1; i < smp_get_cpu_total(); i++) {
        probe_value = 0;
        EXPECT_TRUE(smp_call(i, probe_read, NULL, true));
        EXPECT_TRUE(probe_value == 0x1111);
    }

    // The same address now leads to different memory. A CPU still using
    // its cached lookup would read the old frame.
    EXPECT_TRUE(vmm_unmap_page(NULL, PROBE_VA));
    EXPECT_TRUE(vmm_map_page(NULL, PROBE_VA, (uint64_t)second, VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE));
    for (uint32_t i = 1; i < smp_get_cpu_total(); i++) {
        probe_value = 0;
        EXPECT_TRUE(smp_call(i, probe_read, NULL, true));
        EXPECT_TRUE(probe_value == 0x2222);
    }
    EXPECT_TRUE(*(volatile uint64_t *)PROBE_VA == 0x2222);

    EXPECT_TRUE(vmm_unmap_page(NULL, PROBE_VA));
    EXPECT_TRUE(test_free_frames() == frames_with_tables);
    pmm_free_frame(first);
    pmm_free_frame(second);
}

void test_suite_smp(void) {
    kprintf("Suite: CPUs and Spinlocks\n");
    RUN_TEST(spinlock_basic_test);
    RUN_TEST(smp_cpus_report_in);
    RUN_TEST(per_cpu_data_and_exceptions);
    RUN_TEST(lock_rules_are_checked);
    RUN_TEST(functions_run_on_other_cpus);
    RUN_TEST(cpus_share_the_kernel_safely);
    RUN_TEST(tasks_run_on_every_cpu_at_once);
    RUN_TEST(more_cpus_do_more_work);
    RUN_TEST(other_cpus_forget_removed_kernel_mappings);
}
