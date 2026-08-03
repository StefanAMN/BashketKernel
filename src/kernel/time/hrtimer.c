/**
 * @file hrtimer.c
 * @brief High-resolution time (see hrtimer.h)
 */

#include "hrtimer.h"
#include "spinlock.h"
#include "pit.h"
#include "sched.h"
#include "kprintf.h"

#define CALIBRATION_TICKS 5
#define NS_PER_SECOND 1000000000ULL
#define NS_PER_TICK (NS_PER_SECOND / TIMER_HZ)

static uint64_t tsc_hz = 0;
static uint64_t tsc_at_start = 0;
static uint64_t ticks_at_start = 0;
static uint64_t last_reading = 0;   // Guards against the clock ever stepping back

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// Spin until the tick counter changes, and return the TSC at that moment
static uint64_t tsc_at_next_tick(void) {
    uint64_t tick = pit_get_ticks();
    while (pit_get_ticks() == tick) asm volatile("pause");
    return rdtsc();
}

void hrtimer_init(void) {
    // From the edge of one tick to the edge of another, CALIBRATION_TICKS later
    uint64_t begin = tsc_at_next_tick();
    uint64_t begin_tick = pit_get_ticks();
    while (pit_get_ticks() - begin_tick < CALIBRATION_TICKS) asm volatile("pause");
    uint64_t end = rdtsc();

    uint64_t counts = end - begin;
    uint64_t measured = counts * TIMER_HZ / CALIBRATION_TICKS;

    // A counter that does not advance, or an absurd rate, is not usable
    if (end > begin && measured >= 1000000ULL && measured <= 100000000000ULL) {
        tsc_hz = measured;
    }
    tsc_at_start = rdtsc();
    ticks_at_start = pit_get_ticks();

    if (tsc_hz) {
        kprintf("hrtimer: timestamp counter runs at %lu MHz; clock resolution is nanoseconds\n", tsc_hz / 1000000);
    } else {
        kprintf("hrtimer: timestamp counter unusable; clock resolution is one tick (10 ms)\n");
    }
}

uint64_t hrtimer_tsc_hz(void) {
    return tsc_hz;
}

// Protects last_reading, which every CPU reads and advances
static spinlock_t clock_lock = SPINLOCK_RANKED("clock", LOCK_RANK_TIME);

uint64_t clock_ns(void) {
    uint64_t now;

    if (tsc_hz) {
        // counts * 1e9 would overflow 64 bits after a few seconds at GHz
        // rates, so split into whole seconds and the remainder
        uint64_t counts = rdtsc() - tsc_at_start;
        now = (counts / tsc_hz) * NS_PER_SECOND + (counts % tsc_hz) * NS_PER_SECOND / tsc_hz;
    } else {
        now = (pit_get_ticks() - ticks_at_start) * NS_PER_TICK;
    }

    // Never report a time earlier than one already reported
    uint64_t flags = spinlock_acquire_irqsave(&clock_lock);
    if (now < last_reading) now = last_reading;
    last_reading = now;
    spinlock_release_irqrestore(&clock_lock, flags);
    return now;
}

bool nanosleep(uint64_t nanoseconds) {
    uint64_t deadline = clock_ns() + nanoseconds;

    // Sleep the whole ticks, keeping one in hand: a tick-based sleep ends on
    // a tick boundary, which may be most of a tick late
    uint64_t whole_ticks = nanoseconds / NS_PER_TICK;
    if (whole_ticks > 1 && !sched_sleep_ticks(whole_ticks - 1)) return false;

    // Without a usable TSC the best we can do is one more tick
    if (!tsc_hz) return clock_ns() >= deadline || sched_sleep_ticks(1);

    // Watch the clock for the rest. Other tasks still get the CPU: the timer
    // preempts this loop like any other code.
    while (clock_ns() < deadline) {
        if (sched_interrupted()) return false;
        asm volatile("pause");
    }
    return true;
}
