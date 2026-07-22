#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>

#include <stddef.h>

void kprintf(const char* fmt, ...);

// Format into buf (always NUL-terminated if size > 0, truncated if needed).
// Returns the number of characters stored, not counting the NUL.
size_t ksnprintf(char *buf, size_t size, const char *fmt, ...);

// Drop the kprintf lock unconditionally. Only for fatal paths (panic, CPU
// exception) that must print even if the fault happened inside kprintf.
void kprintf_force_unlock(void);
__attribute__((noreturn)) void panic(const char* msg);

#endif
