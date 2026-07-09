#include "idt.h"
#include "gdt.h"

struct idt_entry idt_entries[IDT_ENTRIES];

// Defined in idt_asm.S.
extern void idt_flush(uint64_t idtr_addr);

static void idt_set_gate(uint8_t vector, void *handler, uint8_t ist, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt_entries[vector].offset_low  = addr & 0xFFFF;
    idt_entries[vector].selector    = KERNEL_CODE_SEL;
    idt_entries[vector].ist         = ist & 0x7;
    idt_entries[vector].type_attr   = type_attr;
    idt_entries[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    idt_entries[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt_entries[vector].reserved    = 0;
}

void idt_init(void) {
    for (int vector = 0; vector < IDT_EXCEPTION_COUNT; vector++) {
        idt_set_gate((uint8_t)vector, isr_stub_table[vector], 0, IDT_TYPE_INTERRUPT_GATE);
    }
    // Vectors 32-255 are intentionally left zeroed (Present=0): no IRQ
    // handlers exist yet, so any hardware interrupt hitting one of them
    // before the PIC is remapped/masked would be a bug we want to see.

    struct idt_ptr idtr = {
        .limit = sizeof(idt_entries) - 1,
        .base  = (uint64_t)&idt_entries,
    };
    idt_flush((uint64_t)&idtr);
}
