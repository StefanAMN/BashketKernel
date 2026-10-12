/* System call interface for user programs.
 * Numbers and error codes mirror src/kernel/syscall/syscall.h. */
#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

#define SYS_READ     0
#define SYS_WRITE    1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_YIELD    4
#define SYS_EXIT     5
#define SYS_GETPID   6
#define SYS_LSEEK    8
#define SYS_MMAP     9
#define SYS_MUNMAP   11
#define SYS_BRK      12
#define SYS_PIPE     22
#define SYS_DUP2     33
#define SYS_SLEEP    35
#define SYS_FORK     57
#define SYS_EXECVE   59
#define SYS_WAITPID  61
#define SYS_KILL     62
#define SYS_UNAME    63
#define SYS_READDIR  78
#define SYS_GETCWD   79
#define SYS_CHDIR    80
#define SYS_MKDIR    83
#define SYS_UNLINK   87
#define SYS_CHMOD    90
#define SYS_CHOWN    92
#define SYS_RENAME   82
#define SYS_UMASK    95
#define SYS_GETUID   102
#define SYS_GETGID   104
#define SYS_SETGID   106
#define SYS_GETEUID  107
#define SYS_SETUID   105
#define SYS_GETPPID  110
#define SYS_TIME     201
#define SYS_SETPGID  109
#define SYS_GETPGID  121
#define SYS_TCSETPGRP 307
#define SYS_TCGETPGRP 308
#define SYS_SIGACTION 13
#define SYS_SIGPROCMASK 14
#define SYS_SIGNAL   300
#define SYS_SIGRETURN 302
#define SYS_TTY_RAW  303
#define SYS_TTY_SIZE 304
#define SYS_FB_INFO  309
#define SYS_REBOOT   310
#define SYS_PING     305
#define SYS_CLOCK_NS 228
#define SYS_REALTIME 311
#define SYS_ISATTY   313
#define SYS_STAT     314
#define SYS_LSTAT    315
#define SYS_FSTAT    316
#define SYS_TRUNCATE  317
#define SYS_FTRUNCATE 318
#define SYS_UTIME     319
#define SYS_SETGROUPS 320
#define SYS_GETGROUPS 321
#define SYS_GETEGID   322
#define SYS_MSYNC     323
#define SYS_SETSID    324
#define SYS_GETSID    325
#define SYS_TTY_TOSTOP 326
#define UTIME_NOW ((unsigned long)-1)   /* utime(): the time now */
#define SYS_SETTIME  312
#define SYS_NANOSLEEP 230
#define SYS_SHMGET   29
#define SYS_SHMAT    30
#define SYS_SHMRM    31
#define SYS_NETCONFIG 306
#define SYS_DUP      32
#define SYS_FCNTL    72
#define SYS_PIPE2    293
#define SYS_POLL     7
#define SYS_LINK     86
#define SYS_SYMLINK  88
#define SYS_READLINK 89
#define SYS_SOCKET   41
#define SYS_CONNECT  42
#define SYS_ACCEPT   43
#define SYS_SHUTDOWN 48
#define SYS_LISTEN   50

#define SOCK_STREAM 1   /* TCP: a connection; read and write */
#define SOCK_DGRAM  2   /* UDP: separate datagrams; sendto and recvfrom */
#define SYS_SENDTO   44
#define SYS_RECVFROM 45
#define SYS_BIND     49
#define SYS_SETPRIORITY 301

#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define E2BIG    7
#define EBADF    9
#define ECHILD  10
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EXDEV   18
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EPIPE   32
#define ENOSYS  38
#define EMSGSIZE     90
#define EADDRINUSE   98
#define ENETDOWN    100
#define ENETUNREACH 101
#define ECONNRESET  104
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define ETXTBSY 26
#define ENOTTY    25
#define EFBIG 27
#define EMLINK 31
#define ENAMETOOLONG 36
#define ELOOP 40
#define ENOTEMPTY 39

#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_CLOEXEC 0x80000  /* Close the descriptor when the process execs a new program */

#define WNOHANG 1          /* waitpid: do not wait; 0 if no child has exited yet */
#define WUNTRACED 2        /* waitpid: also report a child that has been stopped */
#define WIFSTOPPED(status) (((status) & 0x10000) != 0)   /* The child is stopped, not dead... */
#define WSTOPSIG(status)   ((status) & 0xFF)             /* ...by this signal */

#define F_GETFD 1          /* fcntl: read the descriptor's flags */
#define F_SETFD 2          /* fcntl: set them */
#define FD_CLOEXEC 1

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define DT_FILE 1
#define DT_DIR  2
#define DT_LNK  3

/* Where a datagram came from. 10.0.2.2 is the number 0x0A000202. */
struct net_address {
    uint32_t ip;
    uint16_t port;
    uint16_t reserved;
};

/* The network interface's settings (addresses as numbers, like net_address).
 * An ip of 0 means "not configured". */
struct net_config {
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;           /* Where names are looked up */
    uint8_t mac[6];         /* The card's address (cannot be set) */
    uint8_t padding[2];
};

/* One directory entry, filled in by readdir() */
/* poll(): what to wait for on each descriptor, and what was found */
struct pollfd {
    int fd;              /* Negative: this entry is ignored */
    short events;        /* POLLIN, POLLOUT */
    short revents;       /* Filled in: those of events that are true, and POLLERR, POLLHUP, POLLNVAL */
};
#define POLLIN   0x001   /* A read would not wait (data, or the end of it) */
#define POLLOUT  0x004   /* A write would not wait */
#define POLLERR  0x008
#define POLLHUP  0x010   /* The other end is gone */
#define POLLNVAL 0x020   /* Not an open descriptor */

struct dirent {
    uint32_t type;      /* DT_FILE, DT_DIR or DT_LNK */
    uint32_t mode;      /* Permission bits, in octal: owner, group, others; 4 read, 2 write, 1 execute. 04000: set-uid, 02000: set-gid, 01000: sticky */
    uint64_t size;
    uint32_t uid;       /* Owner */
    uint32_t gid;       /* Group */
    uint32_t mtime;     /* Last written: seconds since 1970, UTC (0 if not known) */
    uint32_t reserved;
    char name[128];
};

/* What stat, lstat and fstat report (the kernel's struct vfs_stat) */
#define S_FILE     1
#define S_DIR      2
#define S_LINK     3
#define S_PIPE     4
#define S_SOCKET   5
#define S_TERMINAL 6
struct stat {
    uint32_t type;      /* One of the S_ types above */
    uint32_t mode;      /* Permission bits (as in struct dirent) */
    uint32_t uid, gid;  /* Owner and group */
    uint64_t size;      /* Bytes; a link: its target's length; 0 for pipes, sockets and the terminal */
    uint64_t ino;       /* Its number on its filesystem (ext2: inode; FAT: first cluster); 0: none */
    uint32_t links;     /* Names it has; a directory: 2 plus its subdirectories */
    uint32_t mtime;     /* Last written: seconds since 1970, UTC (0 if not known) */
    uint32_t dev;       /* Which filesystem: 1 memory, 2 FAT, 3 ext2; 0 for pipes, sockets, the terminal */
    uint32_t reserved;  /* (Same dev and ino, nonzero: the same file) */
};

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

/* Raw system call. The syscall instruction overwrites RCX (with the return
 * address) and R11 (with RFLAGS), so the compiler must be told. */
static inline long syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    register long r9 asm("r9") = a6;
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

#define syscall0(n)             syscall6((n), 0, 0, 0, 0, 0, 0)
#define syscall1(n, a)          syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define syscall2(n, a, b)       syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define syscall3(n, a, b, c)    syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define syscall4(n, a, b, c, d) syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define syscall5(n, a, b, c, d, e) syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

#endif
