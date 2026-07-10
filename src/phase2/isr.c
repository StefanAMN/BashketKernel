/**
 * @file isr.c
 * @brief High-Level C Interrupt & Exception Dispatcher
 */

#include "interrupts.h"
#include "kprintf.h"
#include <stddef.h>

// Hardware IRQ Handler function pointer type
typedef void (*irq_handler_t)(struct registers *regs);

static irq_handler_t irq_handlers[16] = {0};

const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

void register_irq_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void isr_handler(struct registers *regs) {
    if (regs->int_no < 32) {
        // CPU Exception (0..31)
        kprintf("\n=================================================\n");
        kprintf("CPU EXCEPTION: %s (Interrupt %d)\n", exception_messages[regs->int_no], regs->int_no);
        kprintf("Error Code: 0x%x\n", regs->err_code);

        if (regs->int_no == 14) {
            uint64_t cr2;
            asm volatile("mov %0, cr2" : "=r"(cr2));
            kprintf("Faulting Address (CR2): 0x%x\n", cr2);
            kprintf("Fault Causes: %s, %s, %s, %s\n",
                    (regs->err_code & 1) ? "Page-protection violation" : "Page not present",
                    (regs->err_code & 2) ? "Write access" : "Read access",
                    (regs->err_code & 4) ? "User mode" : "Kernel mode",
                    (regs->err_code & 16) ? "Instruction fetch" : "Data access");
        }

        kprintf("RIP: 0x%x  CS: 0x%x  RFLAGS: 0x%x\n", regs->rip, regs->cs, regs->rflags);
        kprintf("RSP: 0x%x  SS: 0x%x\n", regs->rsp, regs->ss);
        kprintf("RAX: 0x%x  RBX: 0x%x  RCX: 0x%x  RDX: 0x%x\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
        kprintf("RSI: 0x%x  RDI: 0x%x  RBP: 0x%x\n", regs->rsi, regs->rdi, regs->rbp);
        kprintf("=================================================\n");

        while (1) { asm volatile("cli; hlt"); }
    } else if (regs->int_no >= 32 && regs->int_no < 48) {
        // Hardware IRQ (32..47)
        uint8_t irq = regs->int_no - 32;
        if (irq_handlers[irq] != NULL) {
            irq_handlers[irq](regs);
        }

        // Send EOI (End of Interrupt) to PIC
        if (regs->int_no >= 40) {
            // Send EOI to Slave PIC
            asm volatile("out 0xA0, %0" : : "a"((uint8_t)0x20));
        }
        // Send EOI to Master PIC
        asm volatile("out 0x20, %0" : : "a"((uint8_t)0x20));
    } else {
        kprintf("Unhandled Interrupt Vector: %d\n", regs->int_no);
    }
}
