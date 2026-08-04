#include "test.h"
#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "pit.h"

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

    // Let the dummy tasks run
    TEST_WAIT(task1_counter == 5 && task2_counter == 5, 200);

    EXPECT_EQ(task1_counter, 5);
    EXPECT_EQ(task2_counter, 5);

    // The next test counts them as dead. On another CPU they may still be
    // on their way out after the last increment.
    TEST_WAIT(t1->state == TASK_ZOMBIE && t2->state == TASK_ZOMBIE, 200);
    EXPECT_TRUE(t1->state == TASK_ZOMBIE && t2->state == TASK_ZOMBIE);
}

TEST(zombie_reaping) {
    // dummy1 and dummy2 from the previous test have exited by now
    size_t free_with_zombies = pmm_get_free_frames();
    size_t reaped = sched_reap();
    EXPECT_TRUE(reaped >= 2);
    // Each reaped task gives back its 4 kernel stack frames
    EXPECT_TRUE(pmm_get_free_frames() >= free_with_zombies + reaped * 4);
    EXPECT_EQ(sched_reap(), (size_t)0);
}

// Heap stress under preemption: these tasks never yield, so every task switch
// between them is the timer interrupt landing at an arbitrary instruction.
#define STRESS_TASKS 4
#define STRESS_TICKS 30 // Run for 0.3 s, so each task is preempted many times

static volatile int stress_done = 0;
static volatile int stress_corrupt = 0;
static volatile uint64_t stress_deadline = 0;

static void heap_stress_task(void) {
    uint32_t seed = 0x9E3779B9u ^ (uint32_t)(uint64_t)&seed;
    uint8_t *held[8] = {0};
    size_t held_size[8] = {0};

    while (pit_get_ticks() < stress_deadline) {
        seed = seed * 1664525u + 1013904223u;
        size_t slot = (seed >> 8) % 8;

        if (held[slot]) {
            // The block must still hold the pattern written at allocation
            uint8_t want = (uint8_t)(uint64_t)held[slot];
            if (held[slot][0] != want || held[slot][held_size[slot] - 1] != want) {
                stress_corrupt++;
            }
            kfree(held[slot]);
            held[slot] = NULL;
        } else {
            size_t size = 16 + ((seed >> 16) % 2000);
            held[slot] = kmalloc(size);
            if (held[slot]) {
                held_size[slot] = size;
                held[slot][0] = (uint8_t)(uint64_t)held[slot];
                held[slot][size - 1] = (uint8_t)(uint64_t)held[slot];
            }
        }
    }

    for (size_t slot = 0; slot < 8; slot++) {
        if (held[slot]) kfree(held[slot]);
    }
    __atomic_fetch_add(&stress_done, 1, __ATOMIC_SEQ_CST);
}

TEST(heap_stress_under_preemption) {
    sched_reap();
    size_t heap_before = kheap_get_allocated_bytes();
    stress_deadline = pit_get_ticks() + STRESS_TICKS;

    for (int i = 0; i < STRESS_TASKS; i++) {
        EXPECT_TRUE(task_create("heap_stress", heap_stress_task) != NULL);
    }
    while (stress_done < STRESS_TASKS) {
        sched_yield();
    }

    EXPECT_EQ(stress_corrupt, 0);

    // Once the tasks are reaped, every byte they allocated (and their task
    // structs) must be back: a lost or doubled list update would show here.
    // A task bumps stress_done just before it exits, so give the last ones
    // a moment to actually become zombies.
    size_t reaped = 0;
    TEST_WAIT((reaped += sched_reap()) >= STRESS_TASKS, 200);
    EXPECT_EQ(reaped, (size_t)STRESS_TASKS);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

void test_suite_sched(void) {
    kprintf("Suite: Phase 4 Multitasking & Cooperative Scheduler\n");
    RUN_TEST(multitasking_cooperative);
    RUN_TEST(zombie_reaping);
    RUN_TEST(heap_stress_under_preemption);
}
