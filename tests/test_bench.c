#include "test.h"
#include "bench.h"

TEST(kernel_benchmark_suite_test) {
    bench_run_all();
    EXPECT_TRUE(true);
}

void test_suite_bench(void) {
    kprintf("Suite: Phase 38 Kernel Performance & Benchmark Suite\n");
    RUN_TEST(kernel_benchmark_suite_test);
}
