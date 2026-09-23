/**
 * @file bench.h
 * @brief Small timed benchmarks of kernel primitives
 *
 * Each benchmark repeats one operation for a fixed number of timer ticks and
 * reports how many it managed per second. The timer has 10 ms resolution, so
 * the figures are good to a few percent: enough to see a 2x change, not a 2%
 * one. Under an emulator they describe the emulator as much as the kernel.
 */

#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint64_t memcpy_kib_per_s;      // Copying a 1 MiB block
    uint64_t memset_kib_per_s;      // Filling a 1 MiB block
    uint64_t heap_pairs_per_s;      // kmalloc(64) + kfree
    uint64_t frame_pairs_per_s;     // pmm_alloc_frame + pmm_free_frame
} bench_results_t;

/**
 * @brief Run every benchmark (takes about half a second).
 * @return false if the scratch memory could not be allocated.
 */
bool bench_run(bench_results_t *out);

/**
 * @brief Run the benchmarks and print the results.
 */
void bench_run_all(void);

#endif // BENCH_H
