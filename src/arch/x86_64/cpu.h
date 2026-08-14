/**
 * @file cpu.h
 * @brief CPU feature detection and protection features
 */

#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

// Write `val` to model-specific register `msr` of the calling CPU (a #GP
// if the CPU has no such register)
static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Read model-specific register `msr` of the calling CPU
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Run CPUID.
 * @param regs Receives EAX, EBX, ECX, EDX in that order.
 */
void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]);

/**
 * @brief Read the timestamp counter: a count of CPU cycles (at a constant
 *        rate on any CPU this kernel targets) since reset. Its rate is
 *        measured at boot (hrtimer.h).
 */
static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Turn on the protection features this CPU offers (SMEP, UMIP) and
 *        record which are active.
 */
void cpu_protection_init(void);

/**
 * @brief Turn the same protections on on another CPU: these are bits in
 *        control registers, and every CPU has its own.
 */
void cpu_protection_enable_on_this_cpu(void);

// True once SMAP is switched on. Read by the inline helpers below.
extern bool cpu_smap_enabled;

/**
 * @brief Open and close a window in which ring 0 may touch user memory.
 *
 * With SMAP on, any kernel access to a user page faults unless RFLAGS.AC is
 * set. A system call sets it (stac) only around the few statements that are
 * meant to read or write the caller's buffers, after validating them. A stray
 * pointer dereference anywhere else in the kernel then faults instead of
 * quietly reading or corrupting a program's memory.
 *
 * AC is part of RFLAGS, so it is saved and restored per task by the context
 * switch: a task may sleep inside a window without opening one for others.
 * stac/clac do not exist on CPUs without SMAP, hence the checks.
 *
 * The "memory" clobber matters: without it the compiler does not know these
 * instructions have anything to do with memory, and is free to move a load
 * from user memory to before the stac or after the clac.
 */
static inline void user_access_begin(void) {
    if (cpu_smap_enabled) asm volatile("stac" : : : "cc", "memory");
}

// Close the window user_access_begin opened
static inline void user_access_end(void) {
    if (cpu_smap_enabled) asm volatile("clac" : : : "cc", "memory");
}

/**
 * @brief Space-separated names of the protections in force, for /proc/cpuinfo.
 */
const char *cpu_protection_summary(void);

#endif // CPU_H
