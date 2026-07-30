/**
 * @file pit.c
 * @brief Implementation of 8254 PIT Driver and Preemptive Tick Handler
 */

#include "pit.h"
#include "io.h"
#include "interrupts.h"
#include "sched.h"
#include "kprintf.h"
#include "spinlock.h"
#include "console.h"

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

static volatile uint64_t ticks = 0;

static void pit_irq_handler(struct registers *regs) {
    (void)regs;
    ticks++;

    // Show scrolled output at most 50 times a second (see console.c)
    if (ticks % 2 == 0) console_refresh();

    // Wake tasks whose sleep has ended, then let the scheduler preempt
    sched_wake_sleepers(ticks);
    sched_tick();
}

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 100;

    uint32_t divisor = 1193182 / frequency_hz;

    // Command byte 0x34: Channel 0, Access mode lobyte/hibyte, Mode 2 (Rate Generator).
    //
    // Mode 2 holds the output high and pulses it low for one input clock per
    // period, so there is exactly one rising edge per tick. Mode 3 (square
    // wave, 0x36) was used here before; under QEMU its output is re-asserted
    // while already high, and the I/O APIC delivered every tick twice.
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    // Register IRQ 0 (Interrupt 32)
    register_irq_handler(0, pit_irq_handler);

    kprintf("PIT Init: Timer configured at %u Hz (IRQ 0)\n", frequency_hz);

    // From now on something calls console_refresh regularly
    console_set_deferred_refresh(true);
}

bool sched_sleep_ticks(uint64_t count) {
    if (count == 0) return true;

    uint64_t flags = irq_save();
    uint64_t deadline = ticks + count;
    bool completed = true;
    while (ticks < deadline) {
        if (sched_interrupted()) {
            completed = false;
            break;
        }
        sched_block(NULL, deadline);
    }
    irq_restore(flags);
    return completed;
}

uint64_t pit_get_ticks(void) {
    return ticks;
}
