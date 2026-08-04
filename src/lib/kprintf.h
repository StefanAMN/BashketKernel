#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stdbool.h>

#include <stddef.h>

void kprintf(const char* fmt, ...);

// Format into buf (always NUL-terminated if size > 0, truncated if needed).
// Returns the number of characters stored, not counting the NUL.
size_t ksnprintf(char *buf, size_t size, const char *fmt, ...);

// Drop the kprintf lock unconditionally. Only for fatal paths (panic, CPU
// exception) that must print even if the fault happened inside kprintf.
void kprintf_force_unlock(void);
__attribute__((noreturn)) void panic(const char* msg);

// Set when a CPU stops the kernel for good (a panic or a fatal exception).
// The other CPUs stop at their next timer tick or wait loop instead of
// carrying on with a kernel that has declared itself broken.
extern volatile bool kernel_halted;
void kernel_halt_other_cpus(void);

#endif
