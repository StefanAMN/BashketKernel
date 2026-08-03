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
 * @brief Enable the local APIC of the CPU this runs on (for CPUs other than
 *        the boot CPU, after lapic_init has mapped the registers).
 */
void lapic_enable_on_this_cpu(void);

#endif // LAPIC_H
