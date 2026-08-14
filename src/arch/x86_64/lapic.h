/**
 * @file lapic.h
 * @brief Local APIC (LAPIC) Driver Interface
 */

#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Map the local APIC's registers (at the address IA32_APIC_BASE
 *        gives) and enable the boot CPU's APIC. Call once, after the memory
 *        manager; until then the other lapic_ functions do nothing.
 */
void lapic_init(void);

/**
 * @brief Tell the calling CPU's local APIC that the interrupt being handled
 *        is over, so it delivers the next one of the same or lower priority.
 *        Not for the spurious vector (0xFF), which needs none.
 */
void lapic_eoi(void);

/**
 * @brief The hardware identifier of the calling CPU's local APIC (0..255;
 *        0 before lapic_init). Use with interrupts off, or the task may be
 *        on another CPU by the time the answer is used.
 */
uint32_t lapic_get_id(void);

/**
 * @brief Raise interrupt `vector` on the CPU whose local APIC has this id
 *        (an inter-processor interrupt). Waits, spinning, for the previous
 *        one sent from this CPU to be on its way first.
 */
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector);

/**
 * @brief Measure how fast the local APIC timer counts, against a tick
 *        counter that is already running (interrupts must be on). Spins for
 *        `ticks` + 1 ticks, and stops the calling CPU's timer.
 * @param get_ticks Returns the current tick count.
 * @param ticks     How many ticks to measure over.
 * @return false if the timer did not count (or lapic_init failed).
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
 * @brief Set the calling CPU's timer up to interrupt once per arming, with
 *        `vector`, and leave it stopped.
 */
void lapic_timer_one_shot_on_this_cpu(uint8_t vector);

/**
 * @brief Make the calling CPU's one-shot timer fire after `counts` counts
 *        (see lapic_timer_counts_per_tick), replacing whatever it was set
 *        to. 0 stops it.
 */
void lapic_timer_arm(uint32_t counts);

/**
 * @brief Enable the local APIC of the CPU this runs on (for CPUs other than
 *        the boot CPU, after lapic_init has mapped the registers).
 */
void lapic_enable_on_this_cpu(void);

#endif // LAPIC_H
