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

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    if (!lapic_base) return;

    // Interrupt Command Register (Intel SDM Vol. 3A, 11.6.1): the high half
    // (0x310) names the destination APIC in bits 31..24; writing the low
    // half (0x300) sends. Low half: bits 7..0 vector, delivery mode 000
    // (fixed), destination mode physical, no shorthand. Bit 12 reads as 1
    // while the previous send is still on its way.
    while (lapic_read(0x300) & (1u << 12)) asm volatile("pause");
    lapic_write(0x310, lapic_id << 24);
    lapic_write(0x300, vector);
}

// --- Timer -------------------------------------------------------------------
//
// Each local APIC has a timer that counts down from a programmed value at
// some rate derived from the CPU's bus clock, and interrupts its own CPU.
// The rate is not documented anywhere: it is measured, once, against a
// clock that is (the PIT).

#define LAPIC_LVT_TIMER      0x320
#define LAPIC_TIMER_INITIAL  0x380
#define LAPIC_TIMER_CURRENT  0x390
#define LAPIC_TIMER_DIVIDE   0x3E0
#define LAPIC_TIMER_DIV_16   0x3      // Count once every 16 bus clocks
#define LAPIC_LVT_MASKED     (1u << 16)
#define LAPIC_LVT_PERIODIC   (1u << 17)

static uint32_t timer_counts_per_tick = 0;

bool lapic_timer_calibrate(uint64_t (*get_ticks)(void), uint32_t ticks) {
    if (!lapic_base || ticks == 0) return false;

    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED); // Count, but do not interrupt

    // Start exactly on a tick, then count through `ticks` whole ones
    uint64_t start = get_ticks();
    while (get_ticks() == start) asm volatile("pause");
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);
    start = get_ticks();
    while (get_ticks() < start + ticks) asm volatile("pause");
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0); // Stop

    timer_counts_per_tick = elapsed / ticks;
    return timer_counts_per_tick > 0;
}

uint32_t lapic_timer_counts_per_tick(void) {
    return timer_counts_per_tick;
}

void lapic_timer_start_on_this_cpu(uint8_t vector) {
    if (!lapic_base || timer_counts_per_tick == 0) return;
    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, vector | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INITIAL, timer_counts_per_tick);
}
