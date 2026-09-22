/**
 * @file isr.c
 * @brief High-Level C Interrupt & Exception Dispatcher
 */

#include "interrupts.h"
#include "kprintf.h"
#include "io.h"
#include "pic.h"
#include "lapic.h"
#include "kstack.h"
#include "sched.h"
#include "vmm.h"
#include "process.h"
#include "cpu.h"
#include "console.h"
#include <stddef.h>

typedef void (*irq_handler_t)(struct registers *regs);

static irq_handler_t irq_handlers[16] = {0};

const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

void register_irq_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

// An exception raised by ring 3 code is that program's problem, not the
// kernel's: report it, end the process, keep the system running.
static void kill_faulting_process(struct registers *regs) {
    task_t *task = sched_get_current_task();
    kprintf("\n[pid %u %s] killed: %s at RIP 0x%zx", task->pid, task->name,
            exception_messages[regs->int_no], regs->rip);
    if (regs->int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %0, cr2" : "=r"(cr2));
        kprintf(", address 0x%zx (%s%s)", cr2,
                (regs->err_code & 2) ? "write" : ((regs->err_code & 16) ? "execute" : "read"),
                (regs->err_code & 1) ? ", protection" : ", not mapped");
    }
    kprintf("\n");

    // Shell convention: killed by signal N reports status 128 + N
    task_exit_status(128 + (regs->int_no == 14 || regs->int_no == 13 ? SIGSEGV : SIGKILL));
}

// A write to a present page of a user process may be the first write to a
// copy-on-write page after fork. That is not an error: give the process its
// own copy and let it retry the instruction. This also covers the kernel
// writing to a user buffer on the process's behalf (CR0.WP makes ring 0
// respect read-only pages).
static bool try_copy_on_write(struct registers *regs) {
    if (regs->int_no != 14 || (regs->err_code & 3) != 3) return false; // Not "write to present page"

    task_t *task = sched_get_current_task();
    if (!task || !task->is_user) return false;

    uint64_t cr2;
    asm volatile("mov %0, cr2" : "=r"(cr2));
    uint64_t *space = vmm_space_from_phys((uint64_t)task->pml4_phys);
    if (vmm_handle_cow_fault(space, cr2)) return true;

    // Still a copy-on-write page? Then the copy could not be made: memory is
    // exhausted. fork promised this page without reserving a frame for it,
    // and a store instruction has no way to return an error, so the process
    // has to go. That holds even if the kernel was the one writing (to this
    // process's buffer, during its system call): it is still this process
    // that cannot continue, not a kernel bug.
    if (cr2 < USER_SPACE_END && (vmm_get_pte(space, cr2 & ~0xFFFULL) & VMM_COW)) {
        kprintf("\n[pid %u %s] killed: out of memory copying a shared page at 0x%zx\n",
                task->pid, task->name, cr2);
        task_exit_status(128 + SIGKILL);
    }
    return false;
}

// A fault on a page that is not there, inside the stack region of a user
// process, means the stack needs to grow.
static bool try_grow_stack(struct registers *regs) {
    if (regs->int_no != 14 || (regs->err_code & 1)) return false; // Not "page not present"

    uint64_t cr2;
    asm volatile("mov %0, cr2" : "=r"(cr2));
    return process_grow_stack(cr2);
}

void isr_handler(struct registers *regs) {
    // Handlers never touch user memory. The interrupted code may have had a
    // user access window open (and ring 3 can set RFLAGS.AC itself): close it
    // for the handler. iretq restores the interrupted code's own flags.
    user_access_end();

    if (try_copy_on_write(regs)) return;
    if (try_grow_stack(regs)) return;

    if (regs->int_no < 32 && (regs->cs & 3) == 3) {
        kill_faulting_process(regs);
        return; // Not reached
    }

    if (regs->int_no < 32) {
        // CPU Exception (0..31). The fault may have happened while the
        // interrupted code held the kprintf lock, so take it away first.
        kprintf_force_unlock();
        kprintf("\n=================================================\n");
        kprintf("CPU EXCEPTION: %s (Interrupt %zd)\n", exception_messages[regs->int_no], regs->int_no);
        kprintf("Error Code: 0x%zx\n", regs->err_code);

        if (regs->int_no == 8) {
            kprintf("A double fault usually means the kernel stack is unusable.\n");
            if (kstack_is_guard_page(regs->rsp) || kstack_is_guard_page(regs->rsp - 8)) {
                kprintf("RSP is in a stack guard page: KERNEL STACK OVERFLOW.\n");
            }
        }

        if (regs->int_no == 14) {
            uint64_t cr2;
            asm volatile("mov %0, cr2" : "=r"(cr2));
            kprintf("Faulting Address (CR2): 0x%zx\n", cr2);
            kprintf("Fault Causes: %s, %s, %s, %s\n",
                    (regs->err_code & 1) ? "Page-protection violation" : "Page not present",
                    (regs->err_code & 2) ? "Write access" : "Read access",
                    (regs->err_code & 4) ? "User mode" : "Kernel mode",
                    (regs->err_code & 16) ? "Instruction fetch" : "Data access");
            if (kstack_is_guard_page(cr2)) {
                kprintf("Faulting address is a stack guard page: KERNEL STACK OVERFLOW.\n");
            }
        }

        kprintf("RIP: 0x%zx  CS: 0x%zx  RFLAGS: 0x%zx\n", regs->rip, regs->cs, regs->rflags);
        kprintf("RSP: 0x%zx  SS: 0x%zx\n", regs->rsp, regs->ss);
        kprintf("RAX: 0x%zx  RBX: 0x%zx  RCX: 0x%zx  RDX: 0x%zx\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
        kprintf("RSI: 0x%zx  RDI: 0x%zx  RBP: 0x%zx\n", regs->rsi, regs->rdi, regs->rbp);
        kprintf("=================================================\n");

        console_refresh(); // Interrupts are off for good: show the report now
        while (1) { asm volatile("cli\nhlt"); }
    } else if (regs->int_no >= 32 && regs->int_no < 48) {
        // Send End of Interrupt (EOI) to Local APIC and 8259 PIC before dispatching
        // handler, so preemptive context switches (sched_yield) do not leave the
        // interrupt in-service in the Local APIC.
        lapic_eoi();

        if (regs->int_no >= 40) {
            outb(PIC2_COMMAND, 0x20);
        }
        outb(PIC1_COMMAND, 0x20);

        // Hardware IRQ (32..47)
        uint8_t irq = regs->int_no - 32;
        if (irq_handlers[irq] != NULL) {
            irq_handlers[irq](regs);
        }

        // This interrupt arrived while user code was running and is about to
        // return to it, so the task holds nothing in the kernel: a safe point
        // for a pending kill. (This is what stops a program stuck in a loop.)
        if ((regs->cs & 3) == 3) sched_check_kill();
    } else {
        kprintf("Unhandled Interrupt Vector: %zd\n", regs->int_no);
    }
}
