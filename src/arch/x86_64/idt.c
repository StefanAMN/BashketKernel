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

// Fill gate `num`: the handler's address `base`, the code segment it runs
// in, and the type and attribute byte `flags` (0x8E: present, DPL 0, 64-bit
// interrupt gate). No interrupt stack (IST 0).
static void idt_set_gate(uint8_t num, uint64_t base, uint16_t selector, uint8_t flags) {
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
        // 0x8E = Present (0x80) | DPL 0 | 64-bit Interrupt Gate (0x0E).
        // DPL 0 for every gate: user code cannot raise these with `int n`
        // (it gets #GP instead). System calls use the syscall instruction.
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    }

    // Vector 8 (double fault) runs on the IST1 stack set up in gdt_init
    idt_entries[8].ist = 1;

    // Load IDTR via LIDT instruction
    asm volatile ("lidt [%0]" : : "r"(&idt_pointer));
}

void idt_load_on_this_cpu(void) {
    asm volatile ("lidt [%0]" : : "r"(&idt_pointer));
}
