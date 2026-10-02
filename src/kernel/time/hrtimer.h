/**
 * @file hrtimer.h
 * @brief High-resolution time
 *
 * The timer interrupt arrives 100 times a second, so by counting ticks the
 * kernel only knows the time to the nearest 10 ms. The CPU also has a
 * counter that advances with every clock cycle, the timestamp counter (TSC),
 * readable in a few nanoseconds with one instruction, but in units nobody
 * tells us: cycles of this particular machine.
 *
 * At boot this module measures how many TSC counts pass during a known
 * number of timer ticks. From then on, clock_ns() converts the TSC into
 * nanoseconds since boot.
 */

#ifndef HRTIMER_H
#define HRTIMER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Calibrate the timestamp counter against the timer interrupt.
 *        Takes about 50 ms. Needs interrupts enabled and the timer running.
 */
void hrtimer_init(void);

/**
 * @brief Nanoseconds since hrtimer_init (monotonic: never goes backwards).
 *        Before calibration, or if it failed, falls back to tick resolution.
 */
uint64_t clock_ns(void);

/**
 * @brief Measured TSC frequency in Hz (0 if calibration failed).
 */
uint64_t hrtimer_tsc_hz(void);

/**
 * @brief Sleep for at least this long. Whole ticks are slept without using
 *        the CPU; the remainder (under one tick) is waited out by watching
 *        the clock, so the sleep ends within microseconds of the request
 *        rather than at the next tick.
 * @return true if the full time passed, false if a signal cut it short.
 */
bool nanosleep(uint64_t nanoseconds);

#endif // HRTIMER_H
