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
 * @brief Configure LAPIC Timer for high-precision microsecond interrupts.
 * @param quantum Microsecond count or initial countdown.
 */
void lapic_timer_init(uint32_t quantum);

#endif // LAPIC_H
