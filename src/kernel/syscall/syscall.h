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
#define SYS_DUP         32   // (fd): a second descriptor for the same open file, the lowest free one
#define SYS_DUP2        33
#define SYS_FCNTL       72   // (fd, F_GETFD | F_SETFD, flags): read or set the descriptor's close-on-exec flag
#define SYS_PIPE2       293  // (int fds[2], flags): like pipe; O_CLOEXEC sets close-on-exec on both ends
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
#define SYS_RENAME      82   // (old path, new path): the same filesystem only; refuses to replace an existing entry
#define SYS_MKDIR       83
#define SYS_UNLINK      87
#define SYS_CHMOD       90   // (path, mode): owner or root
#define SYS_CHOWN       92   // (path, uid, gid): root only, except an owner moving a file to their own group; 0xFFFFFFFF leaves one as it is
#define SYS_UMASK       95   // (mask): permission bits removed from files this process creates; returns the previous mask
#define SYS_GETUID      102  // The real user: who started the process
#define SYS_GETGID      104  // The real group: that of whoever started the process
#define SYS_SETUID      105  // (uid): root may become anyone; anyone else may only return to their real user
#define SYS_SETGID      106  // (gid): root sets real and effective group; anyone else may only return to their real group
#define SYS_GETEUID     107  // The effective user: whose rights the process has
#define SYS_SETPGID     109  // (pid, pgid): move a process (0: the caller) into a process group (0: its own)
#define SYS_GETPPID     110
#define SYS_GETPGID     121  // (pid): the process group of a process (0: the caller)
#define SYS_TCSETPGRP   307  // (pgid): make this group the terminal's foreground job (0: none)
#define SYS_TCGETPGRP   308  // (): the terminal's foreground group
#define SYS_CLEAR       100
#define SYS_TIME        201  // Timer ticks since boot
#define SYS_SETPRIORITY 301 // (priority): 0 high, 1 normal, 2 low; for the caller itself
#define SYS_SIGNAL      300  // (sig, handler, restorer): returns the previous handler
#define SYS_SIGACTION   13   // (sig, const struct signal_action *new, struct signal_action *old, restorer); new or old may be NULL
#define SYS_SIGPROCMASK 14   // (how, const uint32_t *set, uint32_t *old); set or old may be NULL
#define SYS_SIGRETURN   302  // Only for the C library's handler-return stub
#define SYS_TTY_RAW     303  // (flag): 1 delivers every key at once without echo; 0 goes back to lines
#define SYS_SHMGET      29   // (key, size): id of the segment with that key, created if new
#define SYS_SHMAT       30   // (id): map the segment into the caller; returns its address
#define SYS_SHMRM       31   // (id): the key is released; memory goes when the last mapping does
#define SYS_PING        305  // (ip, timeout_ms): round-trip time in microseconds, or a negative error
#define SYS_CLOCK_NS    228  // (): nanoseconds since boot
#define SYS_NANOSLEEP   230  // (nanoseconds): sleep with sub-tick precision
#define SYS_NETCONFIG   306  // (struct net_config *get, const struct net_config *set): read the interface settings, then (root only) replace them; either may be NULL
#define SYS_SOCKET      41   // (type): a new socket, as a file descriptor; SOCK_STREAM (TCP) or SOCK_DGRAM (UDP)
#define SYS_CONNECT     42   // (fd, ip, port): open a TCP connection
#define SYS_ACCEPT      43   // (fd, struct net_address *from): the next connection to a listening socket, as a new descriptor
#define SYS_SHUTDOWN    48   // (fd): TCP: nothing more will be written; reading goes on
#define SYS_LISTEN      50   // (fd, backlog): take connections on the bound port

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SYS_SENDTO      44   // (fd, buf, len, ip, port)
#define SYS_RECVFROM    45   // (fd, buf, len, struct net_address *from, timeout_ms); 0 ms waits for ever
#define SYS_BIND        49   // (fd, port)
#define SYS_REBOOT      310  // (int restart): root only; ends every process, marks the disks clean, switches the machine off (0) or restarts it (1); does not return
#define SYS_FB_INFO     309  // (uint32_t info[4]): receives width, height, bytes per row and bits per pixel of the screen (/dev/fb0)
#define SYS_LINK        86   // (existing, path): a second name for a file (ext2)
#define SYS_SYMLINK     88   // (target, path): make path a symbolic link to target
#define SYS_READLINK    89   // (path, buf, size): the target of a symbolic link, not NUL-terminated
#define SYS_POLL        7    // (struct pollfd *fds, count, timeout_ms): wait until one is ready (poll.h); -1 ms: no limit
#define SYS_ISATTY      313  // (fd): 1 if it is the terminal, 0 if not, -EBADF
#define SYS_STAT        314  // (path, struct vfs_stat *): describe what path names, following a final link
#define SYS_LSTAT       315  // (path, struct vfs_stat *): the same, describing a final link itself
#define SYS_FSTAT       316  // (fd, struct vfs_stat *): describe an open file (a pipe, a socket, the terminal too)
#define SYS_TRUNCATE    317  // (path, length): make a file that long (shorter: cut; longer: zeros)
#define SYS_FTRUNCATE   318  // (fd, length): the same for a file open for writing
#define SYS_UTIME       319  // (path, seconds): set a file's modification time; UTIME_NOW: now
#define SYS_SETGROUPS   320  // (count, const uint32_t *list): root only; at most 16; the caller's supplementary groups
#define SYS_GETGROUPS   321  // (size, uint32_t *list): the caller's supplementary groups; returns how many (size 0: only that)
#define SYS_GETEGID     322  // The effective group: whose group rights the process has (differs in a set-gid program)
#define SYS_MSYNC       323  // (addr, length, flags): write what shared mappings in the range wrote to their files
#define SYS_SETSID      324  // (): a session and group of one's own, without a terminal; the session's number
#define SYS_GETSID      325  // (pid): the session of a process (0: the caller); 0 is the console's
#define SYS_TTY_TOSTOP  326  // (on): 1 sets the terminal's tostop (background writers stop), 0 clears it; -1 asks
#define SYS_REALTIME    311  // (): the time of day, nanoseconds since 1970-01-01 UTC
#define SYS_SETTIME     312  // (nanoseconds since 1970): root only; -EINVAL outside 1980..2105
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
