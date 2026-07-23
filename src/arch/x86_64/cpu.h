/**
 * @file cpu.h
 * @brief CPU feature detection and protection features
 */

#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Run CPUID.
 * @param regs Receives EAX, EBX, ECX, EDX in that order.
 */
void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]);

/**
 * @brief Turn on the protection features this CPU offers (SMEP, UMIP) and
 *        record which are active.
 */
void cpu_protection_init(void);

/**
 * @brief Space-separated names of the protections in force, for /proc/cpuinfo.
 */
const char *cpu_protection_summary(void);

#endif // CPU_H
