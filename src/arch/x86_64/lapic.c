/**
 * @file lapic.c
 * @brief Implementation of Local APIC Controller Driver
 */

#include "lapic.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"

static volatile uint32_t *lapic_base = NULL;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
    (void)lapic_base[0]; // Read-after-write flush
}

void lapic_init(void) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t lapic_virt = LAPIC_BASE_PHYS + hhdm;

    // Map LAPIC MMIO physical page into virtual memory space
    vmm_map_page(NULL, lapic_virt, LAPIC_BASE_PHYS, VMM_PRESENT | VMM_WRITABLE);
    lapic_base = (volatile uint32_t *)lapic_virt;

    // Spurious Interrupt Vector Register (SVR 0x0F0)
    // Bit 8 = Enable APIC (0x100), Vector = 0xFF (255)
    lapic_write(0x0F0, 0x1FF);

    kprintf("LAPIC Init: Local APIC enabled (ID: %u, Base MMIO: %p)\n", lapic_get_id(), (void *)lapic_virt);
}

void lapic_enable_on_this_cpu(void) {
    // Each CPU has its own local APIC at the same address
    if (lapic_base != NULL) lapic_write(0x0F0, 0x1FF);
}

void lapic_eoi(void) {
    if (lapic_base) {
        lapic_write(0x0B0, 0); // EOI Register 0x0B0
    }
}

uint32_t lapic_get_id(void) {
    if (!lapic_base) return 0;
    return lapic_read(0x020) >> 24; // ID Register 0x020 bits 31..24
}

void lapic_timer_init(uint32_t quantum) {
    if (!lapic_base) return;

    // Divide Configuration Register (0x3E0) -> Divide by 16 (value 0x03)
    lapic_write(0x3E0, 0x03);

    // LVT Timer Register (0x320) -> Periodic mode (0x20000) | Vector 32 (IRQ 0)
    lapic_write(0x320, 32 | 0x20000);

    // Initial Count Register (0x380)
    lapic_write(0x380, quantum);
}
