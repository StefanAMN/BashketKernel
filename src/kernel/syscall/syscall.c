/**
 * @file syscall.c
 * @brief Implementation of x86_64 System Call Dispatcher and MSR Configuration
 */

#include "syscall.h"
#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include "sched.h"
#include "pit.h"
#include "vfs.h"
#include "keyboard.h"
#include "mmap.h"
#include "process.h"
#include "kstring.h"

extern void syscall_entry(void);

static uint64_t user_brk = 0x00007FFFF0000000ULL;

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init(void) {
    // 1. Enable SCE (System Call Extensions) bit in EFER MSR (0xC0000080)
    uint64_t efer = rdmsr(0xC0000080);
    efer |= 1ULL; // Bit 0 = SCE
    wrmsr(0xC0000080, efer);

    // 2. Configure STAR MSR (0xC0000081)
    // Bits 47..32: Kernel CS (0x08)
    // Bits 63..48: User CS base selector (0x18)
    uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x18 << 48);
    wrmsr(0xC0000081, star);

    // 3. Configure LSTAR MSR (0xC0000082) with syscall_entry target address
    wrmsr(0xC0000082, (uint64_t)syscall_entry);

    // 4. Configure SFMASK MSR (0xC0000084) to mask RFLAGS Interrupt Flag (0x200)
    wrmsr(0xC0000084, 0x200);

    kprintf("Syscall Init: MSRs configured for syscall/sysret (LSTAR: %p)\n", syscall_entry);
}

int64_t syscall_handler(struct registers *regs) {
    uint64_t syscall_num = regs->rax;

    switch (syscall_num) {
        case SYS_READ: {
            int fd = (int)regs->rdi;
            char *buf = (char *)regs->rsi;
            size_t count = (size_t)regs->rdx;

            if (buf == NULL || count == 0) return -1;

            if (fd == 0) { // stdin
                size_t read_bytes = 0;
                while (read_bytes < count) {
                    if (keyboard_has_char()) {
                        buf[read_bytes++] = keyboard_getchar();
                    } else {
                        if (read_bytes > 0) break;
                        sched_yield();
                    }
                }
                return (int64_t)read_bytes;
            } else { // VFS Read
                vnode_t *root = vfs_get_root();
                if (!root) return -1;
                return (int64_t)vfs_read(root, 0, count, buf);
            }
        }

        case SYS_WRITE: {
            int fd = (int)regs->rdi;
            const char *buf = (const char *)regs->rsi;
            size_t count = (size_t)regs->rdx;

            if (buf == NULL) return -1;

            if (fd == 1 || fd == 2) { // stdout or stderr
                for (size_t i = 0; i < count && buf[i] != '\0'; i++) {
                    serial_putchar(buf[i]);
                    console_putchar(buf[i]);
                }
                return (int64_t)count;
            }
            return -1;
        }

        case SYS_OPEN: {
            const char *path = (const char *)regs->rdi;
            if (!path || path[0] == '\0') return -1;
            return 3; // Return file descriptor 3 for valid open path
        }

        case SYS_CLOSE: {
            int fd = (int)regs->rdi;
            return (fd >= 0) ? 0 : -1;
        }

        case SYS_YIELD:
            sched_yield();
            return 0;

        case SYS_EXIT: {
            int status = (int)regs->rdi;
            kprintf("Syscall Exit: Task requested exit with status %d\n", status);
            task_exit();
            return 0;
        }

        case SYS_GETPID:
        case SYS_GETPID_ALT: {
            task_t *curr = sched_get_current_task();
            return curr ? (int64_t)curr->pid : 0;
        }

        case SYS_MMAP: {
            void *addr = (void *)regs->rdi;
            size_t length = (size_t)regs->rsi;
            int prot = (int)regs->rdx;
            int flags = (int)regs->r10;
            int fd = (int)regs->r8;
            int64_t offset = (int64_t)regs->r9;

            void *res = mmap(addr, length, prot, flags, fd, offset);
            return (int64_t)res;
        }

        case SYS_MUNMAP: {
            void *addr = (void *)regs->rdi;
            size_t length = (size_t)regs->rsi;
            return (int64_t)munmap(addr, length);
        }

        case SYS_BRK: {
            uint64_t new_brk = regs->rdi;
            if (new_brk == 0) {
                return (int64_t)user_brk;
            }
            user_brk = new_brk;
            return (int64_t)user_brk;
        }

        case SYS_SLEEP: {
            uint64_t ticks = regs->rdi;
            uint64_t start = pit_get_ticks();
            while (pit_get_ticks() - start < ticks) {
                sched_yield();
            }
            return 0;
        }

        case SYS_FORK:
            return (int64_t)process_fork();

        case SYS_EXECVE: {
            const char *path = (const char *)regs->rdi;
            char *const *argv = (char *const *)regs->rsi;
            char *const *envp = (char *const *)regs->rdx;
            return (int64_t)process_execve_env(path, argv, envp);
        }

        case SYS_WAITPID: {
            pid_t pid = (pid_t)regs->rdi;
            int *status = (int *)regs->rsi;
            return (int64_t)process_waitpid(pid, status);
        }

        case SYS_KILL: {
            pid_t pid = (pid_t)regs->rdi;
            int sig = (int)regs->rsi;
            return (int64_t)process_kill(pid, sig);
        }

        case SYS_UNAME: {
            struct utsname *buf = (struct utsname *)regs->rdi;
            if (!buf) return -1;
            strncpy(buf->sysname, "BashketOS", 64);
            strncpy(buf->nodename, "bashket", 64);
            strncpy(buf->release, "1.0.0", 64);
            strncpy(buf->version, "Phase 5 Syscalls", 64);
            strncpy(buf->machine, "x86_64", 64);
            return 0;
        }

        case SYS_GETCWD: {
            char *buf = (char *)regs->rdi;
            size_t size = (size_t)regs->rsi;
            if (!buf || size < 2) return -1;
            strncpy(buf, "/", size);
            return (int64_t)buf;
        }

        case SYS_CLEAR:
            kprintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
            return 0;

        case SYS_TIME:
            return (int64_t)pit_get_ticks();

        default:
            kprintf("Syscall Warning: Unknown syscall number %zu\n", syscall_num);
            return -1;
    }
}
