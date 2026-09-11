/**
 * @file fpu.c
 * @brief x87 FPU and SSE state management
 */

#include "fpu.h"
#include "kprintf.h"
#include "kstring.h"

#define CR0_MP (1ULL << 1)   // Monitor coprocessor
#define CR0_EM (1ULL << 2)   // Emulation: must be clear, or FPU instructions fault
#define CR0_TS (1ULL << 3)   // Task switched: must be clear, we switch eagerly
#define CR0_NE (1ULL << 5)   // Report x87 errors as exception 16, not via IRQ 13
#define CR4_OSFXSR     (1ULL << 9)  // OS uses FXSAVE/FXRSTOR; enables SSE
#define CR4_OSXMMEXCPT (1ULL << 10) // OS handles SSE exceptions (#XM, vector 19)

#define MXCSR_DEFAULT 0x1F80 // All SSE exceptions masked, round to nearest

static uint8_t clean_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

void fpu_init(void) {
    uint64_t cr0, cr4;

    asm volatile("mov %0, cr0" : "=r"(cr0));
    cr0 |= CR0_MP | CR0_NE;
    cr0 &= ~(CR0_EM | CR0_TS);
    asm volatile("mov cr0, %0" : : "r"(cr0));

    asm volatile("mov %0, cr4" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    asm volatile("mov cr4, %0" : : "r"(cr4));

    // Put the hardware in its reset state and keep a copy: it is what every
    // new program starts with, so nothing leaks from one program to the next.
    uint32_t mxcsr = MXCSR_DEFAULT;
    asm volatile("fninit");
    asm volatile("ldmxcsr [%0]" : : "r"(&mxcsr));
    fpu_save(clean_state);

    kprintf("FPU Init: x87 and SSE enabled; state is saved per user task.\n");
}

void fpu_state_init(uint8_t *state) {
    memcpy(state, clean_state, FPU_STATE_SIZE);
}
