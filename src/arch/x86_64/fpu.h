/**
 * @file fpu.h
 * @brief x87 FPU and SSE state management
 *
 * The kernel itself is compiled without FPU/SSE instructions, so only user
 * programs have floating point state worth keeping. Each user task carries a
 * 512-byte FXSAVE area; the scheduler saves the outgoing task's registers
 * into it and loads the incoming task's on every switch ("eager" switching).
 */

#ifndef FPU_H
#define FPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 512 // Size of an FXSAVE area; must be 16-byte aligned

/**
 * @brief Enable the FPU and SSE and record the clean power-on state.
 */
void fpu_init(void);

/**
 * @brief Fill `state` with the clean state every new program starts from.
 */
void fpu_state_init(uint8_t *state);

/**
 * @brief Save the CPU's FPU/SSE registers into `state`.
 */
static inline void fpu_save(uint8_t *state) {
    asm volatile("fxsave64 [%0]" : : "r"(state) : "memory");
}

/**
 * @brief Load the CPU's FPU/SSE registers from `state`.
 */
static inline void fpu_restore(const uint8_t *state) {
    asm volatile("fxrstor64 [%0]" : : "r"(state) : "memory");
}

#endif // FPU_H
