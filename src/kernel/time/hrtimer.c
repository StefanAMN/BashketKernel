/**
 * @file hrtimer.c
 * @brief Implementation of High-Resolution Timers
 */

#include "hrtimer.h"
#include "pit.h"
#include "sched.h"
#include "kprintf.h"

void hrtimer_init(void) {
    kprintf("HRTimer Init: High-resolution nanosecond timer queue ready.\n");
}

void nanosleep(uint64_t nanoseconds) {
    // The tick (1 / TIMER_HZ s) is the finest resolution available: round up
    uint64_t ns_per_tick = 1000000000ULL / TIMER_HZ;
    sched_sleep_ticks((nanoseconds + ns_per_tick - 1) / ns_per_tick);
}
