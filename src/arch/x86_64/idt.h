/**
 * @file idt.h
 * @brief Interrupt Descriptor Table (IDT) Interface
 * 
 * The IDT maps interrupt vectors (0 to 255) to Handler entry points.
 * Vectors 0–31: CPU Hardware Exceptions (e.g. Page Fault, Divide Error).
 * Vectors 32–47: Hardware IRQs (Timer, Keyboard).
 * Vector 128 (0x80): System Call Gateway.
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
 * @brief Initialize all 256 IDT gates with assembly ISR stubs and load IDTR.
 */
void idt_init(void);

/**
 * @brief Set an individual IDT gate descriptor.
 * @param num Vector number (0..255).
 * @param base 64-bit virtual address of ISR entry stub.
 * @param selector Code Segment Selector (0x08).
 * @param flags Type and attribute flags (0x8E for Ring 0 Interrupt Gate, 0xEE for Ring 3 Trap Gate).
 */
void idt_set_gate(uint8_t num, uint64_t base, uint16_t selector, uint8_t flags);

#endif // IDT_H
