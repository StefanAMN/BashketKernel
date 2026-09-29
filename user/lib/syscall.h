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
#define SYS_GETPPID  110
#define SYS_TIME     201
#define SYS_SIGNAL   300
#define SYS_SIGRETURN 302
#define SYS_SETPRIORITY 301

#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define E2BIG    7
#define EBADF    9
#define ECHILD  10
#define ENOMEM  12
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EPIPE   32
#define ENOSYS  38
#define ENOTEMPTY 39

#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define DT_FILE 1
#define DT_DIR  2

/* One directory entry, filled in by readdir() */
struct dirent {
    uint32_t type;      /* DT_FILE or DT_DIR */
    uint32_t reserved;
    uint64_t size;
    char name[128];
};

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

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

#endif
