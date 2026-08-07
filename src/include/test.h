#ifndef TEST_H
#define TEST_H

#include <stdbool.h>
#include "kprintf.h"

// Globals to track test results
extern int tests_run;
extern int tests_passed;
extern int tests_failed;

#define TEST(name) \
    static void test_##name(void)

#define EXPECT_EQ(actual, expected) \
    do { \
        /* Evaluate each argument once: they may have side effects */ \
        __typeof__(actual) actual_ = (actual); \
        __typeof__(expected) expected_ = (expected); \
        if ((long long)actual_ != (long long)expected_) { \
            kprintf("    [FAIL] %s:%d: Expected %d, got %d\n", __FILE__, __LINE__, (int)expected_, (int)actual_); \
            tests_failed++; \
        } else { \
            tests_passed++; \
        } \
        tests_run++; \
    } while (0)

#define EXPECT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            kprintf("    [FAIL] %s:%d: Condition expected true\n", __FILE__, __LINE__); \
            tests_failed++; \
        } else { \
            tests_passed++; \
        } \
        tests_run++; \
    } while (0)

// Called before each test. A TCP connection finishes closing in the
// background, up to a few seconds after the program that used it has gone,
// and frees its buffers then. A test that counts memory must not start
// while that is still to come.
void test_settle(void);

#define RUN_TEST(name) \
    do { \
        test_settle(); \
        kprintf("  Running %s...\n", #name); \
        test_##name(); \
    } while (0)

// Returns true if every expectation passed.
bool run_all_tests(void);

// Let other tasks run until `condition` holds, for at most `max_ticks` timer
// ticks. "Yield N times" is not a wait: with more than one CPU the other
// tasks run elsewhere, and N yields here can be over before they have begun.
#define TEST_WAIT(condition, max_ticks) do { \
    extern uint64_t timer_ticks(void); \
    extern void sched_yield(void); \
    uint64_t test_wait_deadline = timer_ticks() + (max_ticks); \
    while (!(condition) && timer_ticks() < test_wait_deadline) sched_yield(); \
} while (0)

// For "nothing leaked" checks: frames that are free, plus those the kernel
// heap holds mapped. The heap grows when it must and never shrinks, so a
// workload can leave fewer free frames behind without having leaked any:
// they have become heap, available to the next allocation. When that first
// happens depends on what ran before (and, with several CPUs, in what
// order), so comparing pmm_get_free_frames() alone fails now and then.
static inline size_t test_free_frames(void) {
    extern size_t pmm_get_free_frames(void);
    extern size_t kheap_get_mapped_bytes(void);
    return pmm_get_free_frames() + kheap_get_mapped_bytes() / 4096;
}

#endif // TEST_H
