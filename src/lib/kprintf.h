#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>

void kprintf(const char* fmt, ...);

// Drop the kprintf lock unconditionally. Only for fatal paths (panic, CPU
// exception) that must print even if the fault happened inside kprintf.
void kprintf_force_unlock(void);
__attribute__((noreturn)) void panic(const char* msg);

#endif
