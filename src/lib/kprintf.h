#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>

void kprintf(const char* fmt, ...);
__attribute__((noreturn)) void panic(const char* msg);

#endif
