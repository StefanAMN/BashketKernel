/**
 * @file interrupts.h
 * @brief CPU Register Frame Layout on Interrupt Entry
 * 
 * When an interrupt fires, the CPU automatically pushes:
 * SS, RSP, RFLAGS, CS, RIP (and error code for select exceptions).
 * The common assembly stub pushes RAX..R15 and the interrupt number.
 * This structure maps 1-to-1 with the layout of the CPU stack frame.
 */

#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

struct registers {
    // Pushed by common_isr_stub (Pushed last -> lowest memory address)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // Interrupt Info
    uint64_t int_no;
    uint64_t err_code; // Error code (or dummy 0)

    // Pushed automatically by CPU (Pushed first -> highest memory address)
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

#endif // INTERRUPTS_H
