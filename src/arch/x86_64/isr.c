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

// Names of the 32 exception vectors (Intel SDM Vol. 3A, table 6-1)
static const char *exception_messages[] = {
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

void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs)) {
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

// The signal a program is killed with for exception `vector`, as Unix
// reports them: an arithmetic error is SIGFPE, an instruction the CPU does
// not know is SIGILL, a bad memory access (or privileged instruction) is
// SIGSEGV. None can be caught yet: the process ends with 128 + the signal.
static int exception_signal(uint64_t vector) {
    switch (vector) {
    case 0: case 16: case 19: return SIGFPE;   // #DE, x87 #MF, SIMD #XM
    case 6:                   return SIGILL;   // #UD
    case 1: case 3:           return SIGTRAP;  // #DB, #BP
    case 12: case 13: case 14: case 17: return SIGSEGV; // #SS, #GP, #PF, #AC
    default:                  return SIGKILL;
    }
}

// An exception raised by ring 3 code is that program's problem, not the
// kernel's: report it, end the process (status 128 + the signal of
// exception_signal), keep the system running. Does not return.
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
    task_exit_status(128 + exception_signal(regs->int_no));
}

// A write to a present page of a user process may be the first write to a
// copy-on-write page after fork. That is not an error: give the process its
// own copy and let it retry the instruction. This also covers the kernel
// writing to a user buffer on the process's behalf (CR0.WP makes ring 0
// respect read-only pages). Returns true if the fault was resolved; kills
// the process (does not return) if the copy could not be allocated.
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

// A program's first touch of memory that mmap or brk promised it: now is
// when it gets the frame (see region.h). Only for faults from ring 3: the
// kernel gives a program's buffers their frames before it uses them
// (process_populate), because here it could be holding a lock, and reading
// a file's page from disk sleeps. Returns true if the page is now there;
// kills the process (does not return) when memory ran out.
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
// this one read it, so it is put back. Returns true if the faulting page was
// swapped in; kills the process (does not return) if it could not be.
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

// A fault on a page that is not there, inside the stack region of a user
// process, means the stack needs to grow: give it a zeroed page. Returns
// true if it did (see process_grow_stack for the limits).
static bool try_grow_stack(struct registers *regs) {
    if (regs->int_no != 14 || (regs->err_code & 1)) return false; // Not "page not present"

    uint64_t cr2;
    asm volatile("mov %0, cr2" : "=r"(cr2));
    return process_grow_stack(cr2);
}

// NMIs taken since the last report, across all CPUs (see nmi_handler)
static volatile uint64_t nmis_unreported;

// Called by nmi_stub (isr_stubs.S), on the NMI stack, with GS already this
// CPU's. An NMI can interrupt anything, a holder of the output locks
// included, so nothing here takes a lock: it counts, and the next ordinary
// interrupt reports it (report_nmis). What sent it is not looked into: on
// QEMU it is the monitor's `nmi`; on a PC it would be a watchdog or a
// hardware error (system control port B, 0x61, would say which).
void nmi_handler(struct registers *regs) {
    (void)regs;
    this_cpu()->nmis++;
    __atomic_add_fetch(&nmis_unreported, 1, __ATOMIC_SEQ_CST);
}

// Print what NMIs have arrived since the last report. Called from ordinary
// interrupt handlers, where printing is allowed.
static void report_nmis(void) {
    uint64_t n = __atomic_exchange_n(&nmis_unreported, 0, __ATOMIC_SEQ_CST);
    if (n) kprintf("NMI received (%lu since the last report); carrying on\n", n);
}

// Called by common_isr_stub (isr_stubs.S) for every vector, with interrupts
// off and the interrupted registers in `regs`. Resolves the page faults that
// are part of normal running (swap, copy-on-write, stack growth, promised
// pages); kills a program that raised any other exception; reports a kernel
// exception and halts the machine; and dispatches device interrupts, the
// inter-processor wake-up and the local timer. Changes to `regs` take effect
// when the stub returns with iretq.
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

        if (nmis_unreported) report_nmis();

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
        if (nmis_unreported) report_nmis();

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
