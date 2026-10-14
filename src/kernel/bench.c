/**
 * @file bench.c
 * @brief Small timed benchmarks of kernel primitives (see bench.h)
 */

#include "bench.h"
#include "kprintf.h"
#include "heap.h"
#include "pmm.h"
#include "pit.h"
#include "kstring.h"

#define BENCH_TICKS 10            // Length of each measurement: 0.1 s at 100 Hz
#define BLOCK_SIZE (1024 * 1024)  // 1 MiB

// Keeps the two blocks from being exactly 1 MiB apart. At that distance each
// source page and its destination page share a slot in QEMU's software TLB
// (it is indexed by the low bits of the page number), every access evicts
// the other, and the copy measures the emulator's TLB refill instead of the
// kernel: 90 MiB/s instead of well over 1000.
#define SPACER_SIZE (148 * 1024)

// Wait for the tick counter to change, so a measurement starts at the
// beginning of a tick rather than somewhere inside one.
static uint64_t wait_for_tick_edge(void) {
    uint64_t start = timer_ticks();
    while (timer_ticks() == start) {
        asm volatile("pause");
    }
    return timer_ticks();
}

static uint64_t per_second(uint64_t count) {
    return count * TIMER_HZ / BENCH_TICKS;
}

bool bench_run(bench_results_t *out) {
    uint8_t *src = (uint8_t *)kmalloc(BLOCK_SIZE);
    void *spacer = kmalloc(SPACER_SIZE);
    uint8_t *dst = (uint8_t *)kmalloc(BLOCK_SIZE);
    kfree(spacer);
    if (!src || !dst) {
        kfree(src);
        kfree(dst);
        return false;
    }
    memset(src, 0xAA, BLOCK_SIZE);

    uint64_t count, start;

    count = 0;
    start = wait_for_tick_edge();
    while (timer_ticks() - start < BENCH_TICKS) {
        memcpy(dst, src, BLOCK_SIZE);
        count++;
    }
    out->memcpy_kib_per_s = per_second(count) * (BLOCK_SIZE / 1024);

    count = 0;
    start = wait_for_tick_edge();
    while (timer_ticks() - start < BENCH_TICKS) {
        memset(dst, (int)count, BLOCK_SIZE);
        count++;
    }
    out->memset_kib_per_s = per_second(count) * (BLOCK_SIZE / 1024);

    kfree(src);
    kfree(dst);

    count = 0;
    start = wait_for_tick_edge();
    while (timer_ticks() - start < BENCH_TICKS) {
        void *p = kmalloc(64);
        kfree(p);
        count++;
    }
    out->heap_pairs_per_s = per_second(count);

    count = 0;
    start = wait_for_tick_edge();
    while (timer_ticks() - start < BENCH_TICKS) {
        void *frame = pmm_alloc_frame();
        if (frame) pmm_free_frame(frame);
        count++;
    }
    out->frame_pairs_per_s = per_second(count);

    return true;
}

void bench_run_all(void) {
    bench_results_t r;
    if (!bench_run(&r)) {
        kprintf("bench: not enough memory for the scratch buffers\n");
        return;
    }

    kprintf("Kernel benchmarks (%u ms each; timer resolution 10 ms):\n", BENCH_TICKS * 1000 / TIMER_HZ);
    kprintf("  memcpy, 1 MiB blocks      %8lu MiB/s\n", r.memcpy_kib_per_s / 1024);
    kprintf("  memset, 1 MiB blocks      %8lu MiB/s\n", r.memset_kib_per_s / 1024);
    kprintf("  kmalloc(64) + kfree       %8lu pairs/s\n", r.heap_pairs_per_s);
    kprintf("  frame alloc + free        %8lu pairs/s\n", r.frame_pairs_per_s);
}
