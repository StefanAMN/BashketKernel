#include "../src/test.h"

TEST(addition) {
    EXPECT_EQ(2 + 2, 4);
    EXPECT_EQ(5 + 5, 10);
}

TEST(subtraction) {
    EXPECT_EQ(10 - 5, 5);
}

TEST(intentional_failure) {
    // This is expected to fail to demonstrate the framework output
    EXPECT_EQ(1 + 1, 3);
}

void test_suite_example(void) {
    kprintf("Suite: Example\n");
    RUN_TEST(addition);
    RUN_TEST(subtraction);
    RUN_TEST(intentional_failure);
}
