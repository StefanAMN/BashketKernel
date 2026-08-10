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
#include "percpu.h"
#include "region.h"
#include "smp.h"
#include "timer.h"
#include "swap.h"
#include <stddef.h>

typedef void (*irq_handler_t)(struct registers *regs);

// PCI devices share interrupt lines (QEMU puts the virtio disk and network
// card on the same one), so each IRQ can have several handlers. All of them
// are called; each asks its own device whether it was the one.
#define HANDLERS_PER_IRQ 4
static irq_handler_t irq_handlers[16][HANDLERS_PER_IRQ] = {0};

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
    if (irq >= 16 || !handler) return;
    for (int i = 0; i < HANDLERS_PER_IRQ; i++) {
        if (irq_handlers[irq][i] == handler) return;       // Already registered
        if (irq_handlers[irq][i] == NULL) {
            irq_handlers[irq][i] = handler;
            return;
        }
    }
    kprintf("IRQ %u: too many handlers, one was not registered\n", irq);
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
// A program's first touch of memory that mmap or brk promised it: now is
// when it gets the frame (see region.h). Only for faults from ring 3: the
// kernel gives a program's buffers their frames before it uses them
// (process_populate), because here it could be holding a lock, and reading
// a file's page from disk sleeps.
static bool try_promised_page(struct registers *regs) {
    if (regs->int_no != 14 || (regs->err_code & 1) || (regs->cs & 3) != 3) return false;

    task_t *task = sched_get_current_task();
    if (!task || !task->is_user) return false;
    uint64_t cr2;
    asm volatile("mov %0, cr2" : "=r"(cr2));

    // Reading a file may take a while: let interrupts in, as in a system
    // call. The frame on this stack is complete, so being preempted is fine.
    asm volatile("sti");
    int result = region_fault(task, cr2);
    asm volatile("cli");

    if (result < 0) {
        kprintf("\n[pid %u %s] killed: out of memory for a promised page at 0x%zx\n", task->pid, task->name, cr2);
        task_exit_status(128 + SIGKILL);
    }
    return result > 0;
}

// With a swap disk: a page fault from user mode is where a process gives up
// pages when memory is short, and where a page that was swapped out is
// read back. Both wait for the disk, so interrupts are let in; another
// task's fault may then overwrite CR2 on this CPU, and the handlers after
// this one read it, so it is put back.
static bool try_swap(struct registers *regs) {
    if (!swap_active() || regs->int_no != 14 || (regs->cs & 3) != 3) return false;
    task_t *task = sched_get_current_task();
    if (!task || !task->is_user) return false;

    uint64_t cr2;
    asm volatile("mov %0, cr2" : "=r"(cr2));
    asm volatile("sti");
    swap_relieve_pressure();
    // Whatever kind of fault this was: making room may just have pushed out
    // the very page the program was writing to (a "write to a present page"
    // fault whose page is present no longer)
    int result = swap_in(vmm_space_from_phys((uint64_t)task->pml4_phys), cr2);
    asm volatile("cli");
    asm volatile("mov cr2, %0" : : "r"(cr2));

    if (result < 0) {
        kprintf("\n[pid %u %s] killed: a swapped-out page at 0x%zx could not be read back\n", task->pid, task->name, cr2);
        task_exit_status(128 + SIGKILL);
    }
    return result > 0;
}

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

    if (try_swap(regs) || try_copy_on_write(regs) || try_grow_stack(regs) || try_promised_page(regs)) {
        // The fault is dealt with and the program is about to carry on. If
        // that is user code, this is a safe point for a pending kill, as
        // after an interrupt: a program that does little but fault (one
        // short of memory) is rarely in user mode when the timer fires, and
        // would otherwise outlive its kill for a long time.
        if ((regs->cs & 3) == 3) sched_check_kill();
        return;
    }

    if (regs->int_no < 32 && (regs->cs & 3) == 3) {
        kill_faulting_process(regs);
        return; // Not reached
    }

    // A breakpoint in kernel code: used as a self-test that a CPU can take
    // an exception and find its own per-CPU data while doing so. Count it,
    // note which CPU the hardware says we are, and carry on after the int3.
    if (regs->int_no == 3) {
        cpu_t *cpu = this_cpu();
        cpu->breakpoints++;
        cpu->breakpoint_lapic_id = lapic_get_id();
        return;
    }

    if (regs->int_no < 32) {
        // CPU Exception (0..31). The fault may have happened while the
        // interrupted code held the kprintf lock, so take it away first.
        kernel_halt_other_cpus();
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
        for (int i = 0; i < HANDLERS_PER_IRQ && irq_handlers[irq][i] != NULL; i++) {
            irq_handlers[irq][i](regs);
        }

        // This interrupt arrived while user code was running and is about to
        // return to it, so the task holds nothing in the kernel: a safe point
        // for a pending kill. (This is what stops a program stuck in a loop.)
        if ((regs->cs & 3) == 3) {
            sched_check_kill();
            signal_deliver(regs); // And for a signal handler to be entered
        }
    } else if (regs->int_no == IPI_VECTOR_WAKE || regs->int_no == IPI_VECTOR_TIMER) {
        lapic_eoi(); // First, as above: the handler may switch tasks

        if (regs->int_no == IPI_VECTOR_WAKE) smp_handle_ipi();
        else timer_interrupt();

        // The same safe point as after a device interrupt
        if ((regs->cs & 3) == 3) {
            sched_check_kill();
            signal_deliver(regs);
        }
    } else {
        kprintf("Unhandled Interrupt Vector: %zd\n", regs->int_no);
    }
}
