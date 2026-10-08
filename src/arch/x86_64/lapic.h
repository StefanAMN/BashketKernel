/**
 * @file lapic.h
 * @brief Local APIC (LAPIC) Driver Interface
 */

#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define LAPIC_BASE_PHYS 0xFEE00000ULL

/**
 * @brief Initialize Local APIC controller for active CPU core.
 */
void lapic_init(void);

/**
 * @brief Send End of Interrupt (EOI) signal to Local APIC.
 */
void lapic_eoi(void);

/**
 * @brief Retrieve unique Local APIC ID of current CPU core.
 * @return LAPIC ID number (0..255).
 */
uint32_t lapic_get_id(void);

/**
 * @brief Raise interrupt `vector` on the CPU whose local APIC has this id
 *        (an inter-processor interrupt).
 */
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector);

/**
 * @brief Measure how fast the local APIC timer counts, against a tick
 *        counter that is already running (interrupts must be on).
 * @param get_ticks Returns the current tick count.
 * @param ticks     How many ticks to measure over.
 */
bool lapic_timer_calibrate(uint64_t (*get_ticks)(void), uint32_t ticks);

/**
 * @brief Timer counts in one tick, as measured (0: not calibrated).
 */
uint32_t lapic_timer_counts_per_tick(void);

/**
 * @brief Make the calling CPU's local APIC interrupt it with `vector` once
 *        per tick. Does nothing before calibration.
 */
void lapic_timer_start_on_this_cpu(uint8_t vector);


/**
 * @brief Enable the local APIC of the CPU this runs on (for CPUs other than
 *        the boot CPU, after lapic_init has mapped the registers).
 */
void lapic_enable_on_this_cpu(void);

#endif // LAPIC_H
