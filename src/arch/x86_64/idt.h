/**
 * @file idt.h
 * @brief Interrupt Descriptor Table (IDT) Interface
 * 
 * The IDT maps interrupt vectors (0 to 255) to Handler entry points.
 * Vectors 0–31: CPU Hardware Exceptions (e.g. Page Fault, Divide Error).
 * Vectors 32–47: Hardware IRQs (Timer, Keyboard).
 * System calls do not use the IDT; they enter through the syscall instruction.
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/**
 * @brief 16-byte 64-bit IDT Gate Descriptor structure per Intel SDM.
 */
struct idt_entry {
    uint16_t offset_low;   // Target ISR offset bits 0..15
    uint16_t selector;     // Target Code Segment Selector in GDT (0x08)
    uint8_t  ist;          // Interrupt Stack Table offset (0 = don't switch stack)
    uint8_t  type_attributes; // Gate Type (Interrupt/Trap) + DPL (Ring 0/3) + Present
    uint16_t offset_middle; // Target ISR offset bits 16..31
    uint32_t offset_high;   // Target ISR offset bits 32..63
    uint32_t zero;          // Reserved
} __attribute__((packed));

/**
 * @brief IDT Pointer structure loaded into CPU via IDTR (`lidt`).
 */
struct idt_ptr {
    uint16_t limit;        // Table size in bytes - 1 (256 * 16 - 1)
    uint64_t base;         // 64-bit linear virtual address of IDT array
} __attribute__((packed));

/**
 * @brief Point all 256 vectors at their stubs in isr_stubs.S (interrupt
 *        gates, DPL 0; the double fault on its own stack, IST1) and load the
 *        table on the boot CPU. Every vector then reaches isr_handler.
 */
void idt_init(void);

/**
 * @brief Load the (shared) IDT on a CPU other than the boot CPU, after
 *        idt_init has filled it.
 */
void idt_load_on_this_cpu(void);

#endif // IDT_H
