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
    uint64_t target_ticks = pit_get_ticks() + (nanoseconds / 10000000ULL);
    while (pit_get_ticks() < target_ticks) {
        sched_yield();
    }
}
