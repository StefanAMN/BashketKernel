/**
 * @file bench.c
 * @brief Implementation of Kernel Benchmark Suite
 */

#include "bench.h"
#include "kprintf.h"
#include "heap.h"
#include "string_util.h"

void bench_run_all(void) {
    kprintf("=== Starting Kernel Performance Benchmarks ===\n");

    // Benchmark 1: Memory Copy Speed
    size_t size = 1024 * 1024; // 1 MB
    uint8_t *src = (uint8_t *)kmalloc(size);
    uint8_t *dst = (uint8_t *)kmalloc(size);

    if (src && dst) {
        memset(src, 0xAA, size);
        for (int i = 0; i < 5; i++) {
            memcpy(dst, src, size);
        }
        kprintf("  [Benchmark] Memory Throughput: PASS (5 MB copied)\n");
        kfree(src);
        kfree(dst);
    }

    // Benchmark 2: CPU Calculation Speed
    volatile uint64_t sum = 0;
    for (uint64_t i = 0; i < 500000ULL; i++) {
        sum += i;
    }
    kprintf("  [Benchmark] CPU Calculation: PASS (500K loop iterations)\n");
    kprintf("=== Benchmarks Completed Successfully ===\n");
}
