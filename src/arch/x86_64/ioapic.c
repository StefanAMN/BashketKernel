/**
 * @file ioapic.c
 * @brief Implementation of I/O APIC Controller Driver
 */

#include "ioapic.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "pic.h"
#include "acpi.h"

static volatile uint32_t *ioapic_regsel = NULL;
static volatile uint32_t *ioapic_iowin = NULL;

static uint32_t ioapic_read(uint32_t reg) {
    *ioapic_regsel = reg;
    return *ioapic_iowin;
}

static void ioapic_write(uint32_t reg, uint32_t val) {
    *ioapic_regsel = reg;
    *ioapic_iowin = val;
}

void ioapic_init(void) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t ioapic_virt = IOAPIC_BASE_PHYS + hhdm;

    vmm_map_page(NULL, ioapic_virt, IOAPIC_BASE_PHYS, VMM_PRESENT | VMM_WRITABLE);
    ioapic_regsel = (volatile uint32_t *)ioapic_virt;
    ioapic_iowin  = (volatile uint32_t *)(ioapic_virt + 0x10);

    uint32_t ver = ioapic_read(0x01);
    uint32_t max_redirection_entries = ((ver >> 16) & 0xFF) + 1;

    // Mask legacy 8259 PIC to avoid IRQ routing conflicts
    pic_disable();

    // Route legacy ISA IRQs to vector 32 + IRQ:
    // IRQ 0 (PIT timer), IRQ 1 (keyboard), IRQ 4 (COM1 serial).
    // Each IRQ is routed through exactly one pin, the one the MADT names.
    ioapic_route_isa_irq(0);
    ioapic_route_isa_irq(1);
    ioapic_route_isa_irq(4);

    kprintf("IOAPIC Init: Controller mapped (Max Redirection Lines: %u)\n", max_redirection_entries);
}

void ioapic_route_isa_irq(uint8_t irq) {
    uint16_t flags;
    uint32_t gsi = acpi_isa_irq_to_gsi(irq, &flags);

    // Redirection entry bit 13 = active low, bit 15 = level triggered.
    // ISA defaults (flags 0) are active high, edge triggered.
    uint32_t mode = 0;
    if ((flags & ACPI_INTI_POLARITY_MASK) == ACPI_INTI_ACTIVE_LOW) mode |= (1u << 13);
    if ((flags & ACPI_INTI_TRIGGER_MASK) == ACPI_INTI_LEVEL_TRIGGERED) mode |= (1u << 15);

    ioapic_route_pin((uint8_t)gsi, 32 + irq, mode, 0);
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t apic_id) {
    ioapic_route_pin(irq, vector, 0, apic_id);
}

void ioapic_route_pin(uint8_t irq, uint8_t vector, uint32_t mode, uint32_t apic_id) {
    if (!ioapic_regsel) return;

    uint32_t reg_low  = 0x10 + (irq * 2);
    uint32_t reg_high = 0x10 + (irq * 2) + 1;

    // 1. Mask interrupt first while configuring high bits (target APIC ID)
    uint32_t high_val = (apic_id & 0xFF) << 24;
    uint32_t masked_low = 0x10000 | mode | vector;

    ioapic_write(reg_low, masked_low);
    ioapic_write(reg_high, high_val);

    // 2. Unmask interrupt
    uint32_t unmasked_low = mode | vector;
    ioapic_write(reg_low, unmasked_low);

    kprintf("IOAPIC: Routed pin %u -> Vector %u (LAPIC ID %u)\n", irq, vector, apic_id);
}
