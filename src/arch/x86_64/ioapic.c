/**
 * @file ioapic.c
 * @brief Implementation of I/O APIC Controller Driver
 *
 * The I/O APIC turns device interrupt lines (its input pins, numbered as
 * Global System Interrupts from its GSI base) into interrupt messages for
 * a local APIC. Each pin has a 64-bit redirection entry: vector, delivery
 * mode, polarity, trigger mode, mask and destination (Intel 82093AA I/O APIC
 * datasheet, section 3.2.4). Registers are reached through an index/data
 * pair: write the register number to IOREGSEL, then read or write IOWIN.
 *
 * Only the first I/O APIC the MADT lists is driven; QEMU and most small
 * machines have one.
 */

#include "ioapic.h"
#include "vmm.h"
#include "kprintf.h"
#include "pic.h"
#include "acpi.h"
#include "smp.h"
#include "percpu.h"

#define IOAPIC_DEFAULT_PHYS 0xFEC00000ULL // Where it is when the firmware says nothing
#define IOAPIC_REG_VERSION  0x01          // Bits 23..16: number of redirection entries - 1
#define IOAPIC_REG_REDIR    0x10          // Entry n: low half at 0x10 + 2n, high half one above

static volatile uint32_t *ioapic_regsel = NULL;
static volatile uint32_t *ioapic_iowin = NULL;
static uint32_t gsi_base = 0;    // The GSI of pin 0
static uint32_t pin_count = 0;   // Redirection entries it has

// Read I/O APIC register `reg`. The index/data pair is not locked: every
// caller runs at boot, on the boot CPU, one at a time.
static uint32_t ioapic_read(uint32_t reg) {
    *ioapic_regsel = reg;
    return *ioapic_iowin;
}

// Write `val` to I/O APIC register `reg` (same rule as ioapic_read)
static void ioapic_write(uint32_t reg, uint32_t val) {
    *ioapic_regsel = reg;
    *ioapic_iowin = val;
}

void ioapic_init(void) {
    uint64_t phys = IOAPIC_DEFAULT_PHYS;
    if (!acpi_ioapic(&phys, &gsi_base)) {
        kprintf("IOAPIC: not described by the firmware; trying 0x%zx\n", phys);
        gsi_base = 0;
    }

    // The registers fit in one page (IOREGSEL at 0, IOWIN at 0x10)
    volatile uint32_t *regs = vmm_map_device(phys, 0x20);
    if (!regs) {
        kprintf("IOAPIC: could not map its registers; no device interrupts\n");
        return;
    }
    ioapic_regsel = regs;
    ioapic_iowin  = regs + 4;
    pin_count = ((ioapic_read(IOAPIC_REG_VERSION) >> 16) & 0xFF) + 1;

    // Mask legacy 8259 PIC to avoid IRQ routing conflicts
    pic_disable();

    // Route legacy ISA IRQs to vector 32 + IRQ:
    // IRQ 0 (PIT timer), IRQ 1 (keyboard), IRQ 4 (COM1 serial).
    // Each IRQ is routed through exactly one pin, the one the MADT names.
    ioapic_route_isa_irq(0);
    ioapic_route_isa_irq(1);
    ioapic_route_isa_irq(4);

    kprintf("IOAPIC Init: Controller at 0x%zx, GSIs %u..%u\n", phys, gsi_base, gsi_base + pin_count - 1);
}

void ioapic_route_isa_irq(uint8_t irq) {
    ioapic_route_isa_irq_to(irq, 0);
}

void ioapic_spread_lines(void) {
    // The serial line first: it carries the most (every scripted test types
    // into it), and so gets the first CPU after the boot CPU
    ioapic_route_isa_irq_to(4, smp_next_interrupt_cpu());    // Serial line (COM1)
    ioapic_route_isa_irq_to(1, smp_next_interrupt_cpu());    // Keyboard
}

void ioapic_route_isa_irq_to(uint8_t irq, uint32_t cpu_id) {
    uint16_t flags;
    uint32_t gsi = acpi_isa_irq_to_gsi(irq, &flags);

    // Redirection entry bit 13 = active low, bit 15 = level triggered.
    // ISA defaults (flags 0) are active high, edge triggered.
    uint32_t mode = 0;
    if ((flags & ACPI_INTI_POLARITY_MASK) == ACPI_INTI_ACTIVE_LOW) mode |= (1u << 13);
    if ((flags & ACPI_INTI_TRIGGER_MASK) == ACPI_INTI_LEVEL_TRIGGERED) mode |= (1u << 15);

    cpu_t *cpu = cpu_get(cpu_id);
    ioapic_route_pin(gsi, 32 + irq, mode, cpu && cpu->active ? cpu->lapic_id : 0);
}

void ioapic_route_pin(uint32_t gsi, uint8_t vector, uint32_t mode, uint32_t apic_id) {
    if (!ioapic_regsel) return;
    if (gsi < gsi_base || gsi - gsi_base >= pin_count) {
        kprintf("IOAPIC: GSI %u is not one of this controller's; not routed\n", gsi);
        return;
    }
    uint32_t pin = gsi - gsi_base;
    uint32_t reg_low  = IOAPIC_REG_REDIR + pin * 2;
    uint32_t reg_high = reg_low + 1;

    // Masked (bit 16) while the destination is changed, then unmasked:
    // the entry is never live half written
    ioapic_write(reg_low, (1u << 16) | mode | vector);
    ioapic_write(reg_high, (apic_id & 0xFF) << 24);
    ioapic_write(reg_low, mode | vector);

    kprintf("IOAPIC: Routed GSI %u -> Vector %u (LAPIC ID %u)\n", gsi, vector, apic_id);
}
