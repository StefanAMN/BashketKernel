#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

// Only vectors 0-31 (CPU exceptions) are populated this slice. 32-255 stay
// zeroed (Present=0) -- no hardware IRQ handlers exist yet (that's PIT/PS2,
// a later Phase 2 slice).
#define IDT_EXCEPTION_COUNT 32

// 64-bit interrupt gate: Present=1 DPL=00 Type=1110. Interrupt gates (as
// opposed to trap gates) clear IF on entry (Intel SDM Vol. 3 Sec. 6.14.2),
// so a second interrupt can't land mid-handler while we're still on this
// exception's stack frame.
#define IDT_TYPE_INTERRUPT_GATE 0x8E

struct __attribute__((packed)) idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;        // bits 0-2 only; rest must be 0
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;   // must be zero
};

struct __attribute__((packed)) idt_ptr {
    uint16_t limit;
    uint64_t base;
};

void idt_init(void);

// Exposed for tests/test_idt.c.
extern struct idt_entry idt_entries[IDT_ENTRIES];
extern void *isr_stub_table[IDT_EXCEPTION_COUNT]; // defined in isr_stubs.S

#endif // IDT_H
