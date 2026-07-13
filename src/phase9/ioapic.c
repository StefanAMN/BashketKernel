/**
 * @file ioapic.c
 * @brief Implementation of I/O APIC Controller Driver
 */

#include "ioapic.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"

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

    kprintf("IOAPIC Init: Controller mapped (Max Redirection Lines: %u)\n", max_redirection_entries);
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t apic_id) {
    if (!ioapic_regsel) return;

    uint32_t reg_low  = 0x10 + (irq * 2);
    uint32_t reg_high = 0x10 + (irq * 2) + 1;

    // Lower 32 bits: Vector number, unmasked (bit 16 = 0)
    uint32_t low_val = vector;
    // Upper 32 bits: Target LAPIC ID (bits 31..24)
    uint32_t high_val = (apic_id & 0xFF) << 24;

    ioapic_write(reg_low, low_val);
    ioapic_write(reg_high, high_val);

    kprintf("IOAPIC: Routed IRQ %u -> Vector %u (LAPIC ID %u)\n", irq, vector, apic_id);
}
