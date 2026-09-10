/**
 * @file syscall.h
 * @brief System call numbers, error codes and dispatcher
 *
 * Calling convention (same as Linux x86_64): number in RAX, arguments in
 * RDI, RSI, RDX, R10, R8, R9, result in RAX. The syscall instruction itself
 * destroys RCX and R11. A negative result is -errno.
 *
 * user/lib/syscall.h mirrors these numbers for user programs.
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"

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
#define SYS_SLEEP       35   // Argument: milliseconds
#define SYS_GETPID_ALT  39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_WAITPID     61
#define SYS_KILL        62
#define SYS_UNAME       63
#define SYS_GETCWD      79
#define SYS_GETPPID     110
#define SYS_CLEAR       100
#define SYS_TIME        201  // Timer ticks since boot

// Error codes (returned negated)
#define ENOENT  2
#define ESRCH   3
#define EBADF   9
#define ECHILD  10
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define ENOSYS  38

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/**
 * @brief Configure EFER, STAR, LSTAR and SFMASK for syscall/sysret.
 */
void syscall_init(void);

/**
 * @brief C half of the syscall path, called from syscall_entry.S.
 * @param regs The caller's saved registers. Syscalls such as execve rewrite it.
 * @return Value placed in the caller's RAX.
 */
int64_t syscall_handler(struct registers *regs);

#endif // SYSCALL_H
