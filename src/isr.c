#include "interrupts.h"
#include "kprintf.h"

// Array of exception messages
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
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

void isr_handler(struct registers *regs) {
    if (regs->int_no < 32) {
        kprintf("\n=================================================\n");
        kprintf("CPU EXCEPTION: %s (Interrupt %d)\n", exception_messages[regs->int_no], regs->int_no);
        kprintf("Error Code: 0x%x\n", regs->err_code);
        kprintf("RIP: 0x%x  CS: 0x%x  RFLAGS: 0x%x\n", regs->rip, regs->cs, regs->rflags);
        kprintf("RSP: 0x%x  SS: 0x%x\n", regs->rsp, regs->ss);
        kprintf("RAX: 0x%x  RBX: 0x%x  RCX: 0x%x  RDX: 0x%x\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
        kprintf("RSI: 0x%x  RDI: 0x%x  RBP: 0x%x\n", regs->rsi, regs->rdi, regs->rbp);
        kprintf("=================================================\n");
        
        // Halt on exception
        while(1) { asm volatile("cli; hlt"); }
    } else {
        // Hardware IRQ (32-255)
        // For now, we don't have IRQ handlers, just print and continue
        kprintf("Unhandled Interrupt: %d\n", regs->int_no);
    }
}
