/**
 * @file pit.h
 * @brief 8253/8254 Programmable Interval Timer (PIT) Driver Interface
 */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>

// Scheduler tick rate. The PIT is the only tick source; anything that
// converts ticks to time must use this constant.
#define TIMER_HZ 100

/**
 * @brief Initialize 8254 PIT Channel 0 at specified frequency (e.g. 100 Hz).
 * @param frequency_hz Frequency in Hertz.
 */
void pit_init(uint32_t frequency_hz);

/**
 * @brief Get total elapsed PIT timer ticks since boot.
 * @return Tick count.
 */
uint64_t pit_get_ticks(void);

#endif // PIT_H
