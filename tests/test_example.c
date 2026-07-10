#include "../src/test.h"

TEST(addition) {
    EXPECT_EQ(2 + 2, 4);
    EXPECT_EQ(5 + 5, 10);
}

TEST(subtraction) {
    EXPECT_EQ(10 - 5, 5);
}

void test_suite_example(void) {
    kprintf("Suite: Example\n");
    RUN_TEST(addition);
    RUN_TEST(subtraction);
}
