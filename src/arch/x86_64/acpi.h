/**
 * @file acpi.h
 * @brief Minimal ACPI table access: just enough of the MADT to route ISA IRQs
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>

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

#endif // ACPI_H
