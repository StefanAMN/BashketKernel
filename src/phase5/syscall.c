/**
 * @file syscall.c
 * @brief Implementation of x86_64 System Call Dispatcher and MSR Configuration
 */

#include "syscall.h"
#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include "sched.h"

extern void syscall_entry(void);

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
    // Bits 63..48: User CS base selector (0x1B)
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

        case SYS_YIELD:
            sched_yield();
            return 0;

        case SYS_EXIT: {
            int status = (int)regs->rdi;
            kprintf("Syscall Exit: Task requested exit with status %d\n", status);
            task_exit();
            return 0;
        }

        default:
            kprintf("Syscall Warning: Unknown syscall number %zu\n", syscall_num);
            return -1;
    }
}
