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

#define RUN_TEST(name) \
    do { \
        kprintf("  Running %s...\n", #name); \
        test_##name(); \
    } while (0)

void run_all_tests(void);

#endif // TEST_H
