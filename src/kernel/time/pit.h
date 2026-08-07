/**
 * @file pit.h
 * @brief 8253/8254 Programmable Interval Timer (PIT) Driver Interface
 */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>
#include "timer.h"   // timer_ticks(): what used to be read from here

// Tick rate: the unit the kernel counts time in. The PIT produces the ticks
// while the kernel boots; after that they are read off the clock (see
// timer.h). Anything that converts ticks to time must use this constant.
#define TIMER_HZ 100

/**
 * @brief Initialize 8254 PIT Channel 0 at specified frequency (e.g. 100 Hz).
 * @param frequency_hz Frequency in Hertz.
 */
void pit_init(uint32_t frequency_hz);

#endif // PIT_H
