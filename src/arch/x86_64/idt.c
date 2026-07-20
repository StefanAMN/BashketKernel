/**
 * @file idt.c
 * @brief Implementation of Interrupt Descriptor Table Setup
 */

#include "idt.h"

// IDT array containing 256 16-byte descriptors
static struct idt_entry idt_entries[256];
static struct idt_ptr idt_pointer;

// Array of 256 ISR stub pointers defined in isr_stubs.S
extern uint64_t isr_stub_table[256];

void idt_set_gate(uint8_t num, uint64_t base, uint16_t selector, uint8_t flags) {
    idt_entries[num].offset_low     = (base & 0xFFFF);
    idt_entries[num].offset_middle  = (base >> 16) & 0xFFFF;
    idt_entries[num].offset_high    = (base >> 32) & 0xFFFFFFFF;

    idt_entries[num].selector        = selector;
    idt_entries[num].ist             = 0;
    idt_entries[num].type_attributes = flags;
    idt_entries[num].zero            = 0;
}

void idt_init(void) {
    idt_pointer.limit = sizeof(idt_entries) - 1;
    idt_pointer.base  = (uint64_t)&idt_entries;

    // Load all 256 ISR stubs into IDT
    for (int i = 0; i < 256; i++) {
        // Default: 0x8E = Present (0x80) | Ring 0 (0x00) | 64-bit Interrupt Gate (0x0E)
        // Note: For int 0x80 (syscall/trap gate from user), we set DPL=3 (0xEE) later.
        uint8_t flags = 0x8E;
        if (i == 0x80) {
            flags = 0xEE; // Allow Ring 3 user execution
        }
        idt_set_gate(i, isr_stub_table[i], 0x08, flags);
    }

    // Vector 8 (double fault) runs on the IST1 stack set up in gdt_init
    idt_entries[8].ist = 1;

    // Load IDTR via LIDT instruction
    asm volatile ("lidt [%0]" : : "r"(&idt_pointer));
}
