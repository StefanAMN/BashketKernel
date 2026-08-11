/**
 * @file syscall.h
 * @brief x86_64 Syscall Subsystem Interface (syscall / sysret)
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"

// Standard & Kernel Syscall Numbers
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_YIELD       4
#define SYS_EXIT        5
#define SYS_GETPID      6
#define SYS_MMAP        9
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_SLEEP       35
#define SYS_GETPID_ALT  39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_WAITPID     61
#define SYS_KILL        62
#define SYS_UNAME       63
#define SYS_GETCWD      79
#define SYS_CLEAR       100
#define SYS_TIME        201

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

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
