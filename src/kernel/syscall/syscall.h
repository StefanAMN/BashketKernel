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
#include "kerrno.h"

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_YIELD       4
#define SYS_EXIT        5
#define SYS_GETPID      6
#define SYS_LSEEK       8
#define SYS_MMAP        9
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_PIPE        22
#define SYS_DUP2        33
#define SYS_SLEEP       35   // Argument: milliseconds
#define SYS_GETPID_ALT  39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_WAITPID     61
#define SYS_KILL        62
#define SYS_UNAME       63
#define SYS_READDIR     78   // (fd, struct dirent_info *): next entry of an open directory
#define SYS_GETCWD      79   // (buf, size): returns the length of the path
#define SYS_CHDIR       80
#define SYS_MKDIR       83
#define SYS_UNLINK      87
#define SYS_CHMOD       90   // (path, mode): owner or root
#define SYS_CHOWN       92   // (path, uid): root only
#define SYS_GETUID      102
#define SYS_SETUID      105  // (uid): root may become anyone; nobody else may change
#define SYS_GETPPID     110
#define SYS_CLEAR       100
#define SYS_TIME        201  // Timer ticks since boot
#define SYS_SETPRIORITY 301 // (priority): 0 high, 1 normal, 2 low; for the caller itself
#define SYS_SIGNAL      300  // (sig, handler, restorer): returns the previous handler
#define SYS_SIGRETURN   302  // Only for the C library's handler-return stub
#define SYS_TTY_RAW     303  // (flag): 1 delivers every key at once without echo; 0 goes back to lines
#define SYS_SHMGET      29   // (key, size): id of the segment with that key, created if new
#define SYS_SHMAT       30   // (id): map the segment into the caller; returns its address
#define SYS_SHMRM       31   // (id): the key is released; memory goes when the last mapping does
#define SYS_PING        305  // (ip, timeout_ms): round-trip time in microseconds, or a negative error
#define SYS_CLOCK_NS    228  // (): nanoseconds since boot
#define SYS_NANOSLEEP   230  // (nanoseconds): sleep with sub-tick precision
#define SYS_SOCKET      41   // (): a new UDP socket, as a file descriptor
#define SYS_SENDTO      44   // (fd, buf, len, ip, port)
#define SYS_RECVFROM    45   // (fd, buf, len, struct net_address *from, timeout_ms); 0 ms waits for ever
#define SYS_BIND        49   // (fd, port)
#define SYS_TTY_SIZE    304  // (uint32_t size[2]): receives columns and rows of the screen

// Where a datagram came from, as recvfrom reports it. Addresses are numbers
// with the first byte of the dotted form most significant: 10.0.2.2 is
// 0x0A000202.
struct net_address {
    uint32_t ip;
    uint16_t port;
    uint16_t reserved;
};

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
 * @brief Set the same MSRs on another CPU: each CPU has its own.
 */
void syscall_enable_on_this_cpu(void);

/**
 * @brief C half of the syscall path, called from syscall_entry.S.
 * @param regs The caller's saved registers. Syscalls such as execve rewrite it.
 * @return Value placed in the caller's RAX.
 */
int64_t syscall_handler(struct registers *regs);

#endif // SYSCALL_H
