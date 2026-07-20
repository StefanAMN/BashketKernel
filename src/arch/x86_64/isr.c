/**
 * @file isr.c
 * @brief High-Level C Interrupt & Exception Dispatcher
 */

#include "interrupts.h"
#include "kprintf.h"
#include "io.h"
#include "pic.h"
#include "lapic.h"
#include "kstack.h"
#include <stddef.h>

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
        // CPU Exception (0..31). The fault may have happened while the
        // interrupted code held the kprintf lock, so take it away first.
        kprintf_force_unlock();
        kprintf("\n=================================================\n");
        kprintf("CPU EXCEPTION: %s (Interrupt %zd)\n", exception_messages[regs->int_no], regs->int_no);
        kprintf("Error Code: 0x%zx\n", regs->err_code);

        if (regs->int_no == 8) {
            kprintf("A double fault usually means the kernel stack is unusable.\n");
            if (kstack_is_guard_page(regs->rsp) || kstack_is_guard_page(regs->rsp - 8)) {
                kprintf("RSP is in a stack guard page: KERNEL STACK OVERFLOW.\n");
            }
        }

        if (regs->int_no == 14) {
            uint64_t cr2;
            asm volatile("mov %0, %%cr2" : "=r"(cr2));
            kprintf("Faulting Address (CR2): 0x%zx\n", cr2);
            kprintf("Fault Causes: %s, %s, %s, %s\n",
                    (regs->err_code & 1) ? "Page-protection violation" : "Page not present",
                    (regs->err_code & 2) ? "Write access" : "Read access",
                    (regs->err_code & 4) ? "User mode" : "Kernel mode",
                    (regs->err_code & 16) ? "Instruction fetch" : "Data access");
            if (kstack_is_guard_page(cr2)) {
                kprintf("Faulting address is a stack guard page: KERNEL STACK OVERFLOW.\n");
            }
        }

        kprintf("RIP: 0x%zx  CS: 0x%zx  RFLAGS: 0x%zx\n", regs->rip, regs->cs, regs->rflags);
        kprintf("RSP: 0x%zx  SS: 0x%zx\n", regs->rsp, regs->ss);
        kprintf("RAX: 0x%zx  RBX: 0x%zx  RCX: 0x%zx  RDX: 0x%zx\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
        kprintf("RSI: 0x%zx  RDI: 0x%zx  RBP: 0x%zx\n", regs->rsi, regs->rdi, regs->rbp);
        kprintf("=================================================\n");

        while (1) { asm volatile("cli\nhlt"); }
    } else if (regs->int_no >= 32 && regs->int_no < 48) {
        // Send End of Interrupt (EOI) to Local APIC and 8259 PIC before dispatching
        // handler, so preemptive context switches (sched_yield) do not leave the
        // interrupt in-service in the Local APIC.
        lapic_eoi();

        if (regs->int_no >= 40) {
            outb(PIC2_COMMAND, 0x20);
        }
        outb(PIC1_COMMAND, 0x20);

        // Hardware IRQ (32..47)
        uint8_t irq = regs->int_no - 32;
        if (irq_handlers[irq] != NULL) {
            irq_handlers[irq](regs);
        }
    } else {
        kprintf("Unhandled Interrupt Vector: %zd\n", regs->int_no);
    }
}
