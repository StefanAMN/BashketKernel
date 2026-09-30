/**
 * @file ioapic.h
 * @brief I/O APIC Interrupt Controller Interface
 */

#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Map the I/O APIC the MADT describes (after acpi_init), mask the
 *        8259 PIC for good, and route the timer, keyboard and serial IRQs.
 *        Without it, device interrupts are never delivered.
 */
void ioapic_init(void);

/**
 * @brief Route a legacy ISA IRQ to vector 32 + irq on the boot CPU, using the
 *        pin, polarity and trigger mode the ACPI MADT gives for it. (PCI
 *        devices use this too, for the line their configuration space names:
 *        on QEMU the MADT overrides those lines to level-triggered. Real
 *        PCI routing would need the DSDT's _PRT, which is not read.)
 *        Boot only, one CPU at a time (the registers are an index/data pair).
 */
void ioapic_route_isa_irq(uint8_t irq);

/**
 * @brief ioapic_route_isa_irq, to CPU `cpu_id` (our numbering, cpu_get)
 *        instead of the boot CPU. Its handler then runs there.
 */
void ioapic_route_isa_irq_to(uint8_t irq, uint32_t cpu_id);

/**
 * @brief Once the other CPUs run: send the serial line's and the keyboard's
 *        interrupts to CPUs of their own (smp_next_interrupt_cpu, the serial
 *        line first: on two or more CPUs it gets CPU 1), as the lines routed
 *        later are. The PIT stays with the boot CPU, whose
 *        periodic tick it is when there is no timestamp counter.
 */
void ioapic_spread_lines(void);

/**
 * @brief Program one I/O APIC redirection entry and unmask it. Refused, with
 *        a message, for a GSI this I/O APIC does not handle.
 * @param gsi Global System Interrupt (the I/O APIC's GSI base + its pin).
 * @param vector IDT vector to deliver.
 * @param mode Extra low-word bits (bit 13 active low, bit 15 level triggered).
 * @param apic_id Destination Local APIC ID.
 */
void ioapic_route_pin(uint32_t gsi, uint8_t vector, uint32_t mode, uint32_t apic_id);

#endif // IOAPIC_H
