/**
 * @file fpu.c
 * @brief Implementation of FPU & SSE State Management
 */

#include "fpu.h"
#include "kprintf.h"

void fpu_init(void) {
    uint64_t cr0, cr4;

    // Read CR0, set Monitor Coprocessor (MP, bit 1), clear Emulation (EM, bit 2)
    asm volatile("mov %0, cr0" : "=r"(cr0));
    cr0 |= (1 << 1);  // MP
    cr0 &= ~(1 << 2); // EM
    asm volatile("mov cr0, %0" : : "r"(cr0));

    // Read CR4, set OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
    asm volatile("mov %0, cr4" : "=r"(cr4));
    cr4 |= (1 << 9);  // OSFXSR
    cr4 |= (1 << 10); // OSXMMEXCPT
    asm volatile("mov cr4, %0" : : "r"(cr4));

    // Initialize x87 FPU state
    asm volatile("fninit");

    kprintf("FPU Init: x87 FPU, MMX, and SSE hardware extensions enabled.\n");
}
