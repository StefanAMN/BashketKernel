#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

// This struct must perfectly match what `common_isr_stub` pushes to the stack
struct registers {
    // Data segment (pushed by our stub)
    uint64_t ds;

    // Registers pushed by common_isr_stub (push r15, r14... rax)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // Interrupt number and error code (pushed by individual ISR stubs)
    uint64_t int_no;
    uint64_t err_code;

    // Pushed by the CPU automatically when the interrupt happens
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

void isr_handler(struct registers *regs);

#endif
