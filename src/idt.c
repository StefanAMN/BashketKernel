#include "idt.h"

// We need 256 entries in the IDT
struct idt_entry idt[256];
struct idtr idtr_ptr;

void idt_set_gate(uint8_t vector, void *isr, uint8_t flags) {
    uint64_t addr = (uint64_t)isr;

    idt[vector].isr_low    = (uint16_t)(addr & 0xFFFF);
    idt[vector].kernel_cs  = 0x08; // The Kernel Code Segment we defined in our GDT
    idt[vector].ist        = 0;    // Not using Interrupt Stack Tables yet
    idt[vector].attributes = flags;
    idt[vector].isr_mid    = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].isr_high   = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].reserved   = 0;
}

// These are defined in isr_stubs.S
extern void* isr_stub_table[];

void idt_init(void) {
    idtr_ptr.limit = sizeof(idt) - 1;
    idtr_ptr.base  = (uint64_t)&idt;

    // Zero out the IDT
    uint8_t *idt_ptr = (uint8_t*)&idt;
    for (uint32_t i = 0; i < sizeof(idt); i++) {
        idt_ptr[i] = 0;
    }

    // Populate the 256 IDT gates with our ISR stubs.
    // 0x8E means: Present (0x80) | Ring 0 (0x00) | Interrupt Gate (0x0E).
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x8E);
    }

    // Load the IDT
    asm volatile ("lidt %0" : : "m"(idtr_ptr));
}
