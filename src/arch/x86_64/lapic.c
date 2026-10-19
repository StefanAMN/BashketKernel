/**
 * @file lapic.c
 * @brief Implementation of Local APIC Controller Driver
 */

#include "lapic.h"
#include "vmm.h"
#include "cpu.h"
#include "kprintf.h"

// IA32_APIC_BASE (Intel SDM Vol. 3A, 11.4.4): bits 12 and up hold the
// physical address of the local APIC's registers (the same on every CPU)
#define MSR_APIC_BASE      0x1B
#define APIC_BASE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define LAPIC_REG_ID   0x020
#define LAPIC_REG_EOI  0x0B0
#define LAPIC_REG_SVR  0x0F0   // Spurious interrupt vector register: bit 8 enables the APIC
#define LAPIC_SVR_ON   0x1FF   // Enabled, spurious interrupts on vector 0xFF
#define LAPIC_REG_ICR_LOW  0x300
#define LAPIC_REG_ICR_HIGH 0x310

static volatile uint32_t *lapic_base = NULL;

// Read the local APIC register at byte offset `reg` (of the calling CPU's
// own APIC: every CPU sees its own at the same address)
static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

// Write `val` to the local APIC register at byte offset `reg`, then read
// one back so the write has reached the APIC before the caller goes on
static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
    (void)lapic_base[LAPIC_REG_ID / 4]; // Read-after-write flush
}

void lapic_init(void) {
    uint64_t phys = rdmsr(MSR_APIC_BASE) & APIC_BASE_ADDR_MASK;

    // The registers are one 4 KiB page, mapped uncached
    lapic_base = vmm_map_device(phys, 0x1000);
    if (!lapic_base) {
        kprintf("LAPIC: could not map its registers\n");
        return;
    }
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ON);

    kprintf("LAPIC Init: Local APIC enabled (ID: %u, registers at 0x%zx)\n", lapic_get_id(), phys);
}

void lapic_enable_on_this_cpu(void) {
    // Each CPU has its own local APIC at the same address
    if (lapic_base != NULL) lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ON);
}

void lapic_eoi(void) {
    if (lapic_base) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

uint32_t lapic_get_id(void) {
    if (!lapic_base) return 0;
    return lapic_read(LAPIC_REG_ID) >> 24; // Bits 31..24
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    if (!lapic_base) return;

    // Interrupt Command Register (Intel SDM Vol. 3A, 11.6.1): the high half
    // (0x310) names the destination APIC in bits 31..24; writing the low
    // half (0x300) sends. Low half: bits 7..0 vector, delivery mode 000
    // (fixed), destination mode physical, no shorthand. Bit 12 reads as 1
    // while the previous send is still on its way.
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1u << 12)) asm volatile("pause");
    lapic_write(LAPIC_REG_ICR_HIGH, lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, vector);
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

void lapic_timer_one_shot_on_this_cpu(uint8_t vector) {
    if (!lapic_base || timer_counts_per_tick == 0) return;
    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, vector); // Bits 17 and 18 clear: one shot (Intel SDM Vol. 3A, 11.5.4)
    lapic_write(LAPIC_TIMER_INITIAL, 0);  // Not counting until armed
}

void lapic_timer_arm(uint32_t counts) {
    // Writing the initial count starts the countdown afresh; writing 0
    // stops the timer
    lapic_write(LAPIC_TIMER_INITIAL, counts);
}

void lapic_timer_start_on_this_cpu(uint8_t vector) {
    if (!lapic_base || timer_counts_per_tick == 0) return;
    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, vector | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INITIAL, timer_counts_per_tick);
}
