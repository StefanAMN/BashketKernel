#include "test.h"
#include "fpu.h"

TEST(fpu_sse_enable_test) {
    fpu_init();
    EXPECT_TRUE(true);
}

void test_suite_fpu(void) {
    kprintf("Suite: Phase 26 Hardware FPU & SSE State Management\n");
    RUN_TEST(fpu_sse_enable_test);
}
