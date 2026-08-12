/**
 * @file acpi.h
 * @brief ACPI table access: the MADT (to route ISA IRQs) and the FADT and DSDT
 *        (to power the machine off and reset it)
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// MADT Interrupt Source Override flags (ACPI spec, MPS INTI flags)
#define ACPI_INTI_POLARITY_MASK   0x3
#define ACPI_INTI_ACTIVE_LOW      0x3
#define ACPI_INTI_TRIGGER_MASK    0xC
#define ACPI_INTI_LEVEL_TRIGGERED 0xC

/**
 * @brief Parse the MADT and record its Interrupt Source Overrides.
 * @param rsdp Virtual address of the RSDP as handed over by Limine.
 * @return true if a MADT was found.
 */
bool acpi_init(void *rsdp);

/**
 * @brief Translate a legacy ISA IRQ (0..15) to its Global System Interrupt.
 *
 * ISA IRQs map 1:1 onto GSIs unless the MADT says otherwise. The usual
 * exception is the PIT: IRQ 0 is wired to GSI 2 on QEMU and most PCs.
 *
 * @param irq ISA IRQ number.
 * @param out_flags If non-NULL, receives the override's MPS INTI flags (0 if none).
 * @return The GSI, which is the I/O APIC input pin when there is one I/O APIC.
 */
uint32_t acpi_isa_irq_to_gsi(uint8_t irq, uint16_t *out_flags);

/**
 * @brief Find the \_S5 object ("what to write to switch off") in AML bytecode
 *        and return its two sleep types (for PM1a and PM1b). The bytes come
 *        from the firmware: nothing is read past `length`.
 * @return false if there is no such object, or it is not in a form this
 *         simple search understands.
 */
bool acpi_parse_s5(const uint8_t *aml, size_t length, uint8_t *typ_a, uint8_t *typ_b);

/**
 * @brief Does the firmware describe how to switch the machine off?
 */
bool acpi_can_power_off(void);

/**
 * @brief Switch the machine off (ACPI sleep state S5). Returns only if that
 *        failed or is not possible.
 */
void acpi_power_off(void);

/**
 * @brief Reset the machine: through the FADT's reset register if there is
 *        one, else through the keyboard controller. Returns only if both
 *        failed.
 */
void acpi_reset(void);

#endif // ACPI_H
