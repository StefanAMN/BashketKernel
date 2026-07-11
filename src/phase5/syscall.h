/**
 * @file syscall.h
 * @brief x86_64 Syscall Subsystem Interface (syscall / sysret)
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"

// Syscall Numbers
#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_EXIT   3

/**
 * @brief Initialize MSR registers (EFER, STAR, LSTAR, SFMASK) for syscall/sysret.
 */
void syscall_init(void);

/**
 * @brief C handler invoked by assembly syscall_entry stub.
 * @param regs Pointer to saved register state on kernel stack.
 * @return Syscall return value passed back in RAX to caller.
 */
int64_t syscall_handler(struct registers *regs);

#endif // SYSCALL_H
