/**
 * @file fpu.h
 * @brief Floating Point Unit (FPU) & SSE Initialization Interface
 */

#ifndef FPU_H
#define FPU_H

#include <stdint.h>

/**
 * @brief Enable x87 FPU, MMX, SSE, and SSE2 hardware capabilities.
 */
void fpu_init(void);

#endif // FPU_H
