/**
 * @file ioapic.h
 * @brief I/O APIC Interrupt Controller Interface
 */

#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define IOAPIC_BASE_PHYS 0xFEC00000ULL

/**
 * @brief Initialize I/O APIC controller.
 */
void ioapic_init(void);

/**
 * @brief Route a hardware IRQ line to a specific LAPIC interrupt vector.
 * @param irq Hardware IRQ line number (e.g. 1 for Keyboard).
 * @param vector Target CPU interrupt vector (e.g. 33).
 * @param apic_id Target LAPIC ID (e.g. 0).
 */
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t apic_id);

/**
 * @brief Route a legacy ISA IRQ to vector 32 + irq on the boot CPU, using the
 * pin, polarity and trigger mode the ACPI MADT gives for it.
 */
void ioapic_route_isa_irq(uint8_t irq);

/**
 * @brief Program one I/O APIC redirection entry.
 * @param pin I/O APIC input pin (GSI).
 * @param vector IDT vector to deliver.
 * @param mode Extra low-word bits (bit 13 active low, bit 15 level triggered).
 * @param apic_id Destination Local APIC ID.
 */
void ioapic_route_pin(uint8_t pin, uint8_t vector, uint32_t mode, uint32_t apic_id);

#endif // IOAPIC_H
