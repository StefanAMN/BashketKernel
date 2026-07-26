#include "test.h"
#include "bench.h"
#include "heap.h"
#include "pmm.h"
#include "pit.h"

TEST(benchmarks_measure_something) {
    size_t heap_before = kheap_get_allocated_bytes();
    // The heap grows for the scratch buffers and afterwards keeps a little
    // of that as a reserve, so count frames the heap holds as accounted for
    size_t frames_before = pmm_get_free_frames() + kheap_get_mapped_bytes() / 4096;
    uint64_t ticks_before = pit_get_ticks();

    bench_results_t r = {0};
    EXPECT_TRUE(bench_run(&r));

    // Four measurements of 10 ticks each really took that long
    uint64_t elapsed = pit_get_ticks() - ticks_before;
    EXPECT_TRUE(elapsed >= 40 && elapsed <= 60);

    // Loose sanity bounds: slower than this and something is broken (a byte
    // loop under an emulator still manages tens of MiB/s); faster and the
    // loop did not do the work.
    EXPECT_TRUE(r.memcpy_kib_per_s >= 10 * 1024);
    EXPECT_TRUE(r.memset_kib_per_s >= 10 * 1024);
    EXPECT_TRUE(r.memcpy_kib_per_s < 1000ULL * 1024 * 1024);
    EXPECT_TRUE(r.heap_pairs_per_s >= 1000);
    EXPECT_TRUE(r.frame_pairs_per_s >= 1000);

    // The benchmarks give back everything they used
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
    size_t frames_after = pmm_get_free_frames() + kheap_get_mapped_bytes() / 4096;
    EXPECT_TRUE(frames_after + 2 >= frames_before);   // Up to two new page-table frames for the heap range
    EXPECT_TRUE(frames_after <= frames_before);

    bench_run_all();   // The printing path
}

void test_suite_bench(void) {
    kprintf("Suite: Benchmarks\n");
    RUN_TEST(benchmarks_measure_something);
}
