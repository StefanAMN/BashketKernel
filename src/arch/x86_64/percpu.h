/**
 * @file percpu.h
 * @brief Per-CPU data
 *
 * Some state exists once per CPU rather than once per machine: which task is
 * running here, which kernel stack a system call on this CPU should use,
 * this CPU's task state segment. Each CPU has a `cpu_t` for it.
 *
 * A CPU finds its own `cpu_t` through the GS segment register: in kernel
 * mode the hidden "GS base" holds the address of this CPU's structure, so
 * `gs:[0]` reads its first field from any code on any CPU, with no lookup.
 *
 * User programs may use GS for their own purposes, so the base is switched
 * at every crossing between ring 3 and the kernel with `swapgs`, which
 * exchanges the GS base with a second register kept for this purpose
 * (KERNEL_GS_BASE). The invariant, which every entry and exit path must
 * keep:
 *
 *     in ring 0:  GS base = this CPU's cpu_t,  KERNEL_GS_BASE = the user's value
 *     in ring 3:  GS base = the user's value,  KERNEL_GS_BASE = this CPU's cpu_t
 *
 * Every program's own GS base is 0: nothing lets a program change it (no
 * system call for it, and CR4.FSGSBASE is off). So the "user's value" a CPU
 * holds is the same whichever task it belongs to, and a task that enters the
 * kernel on one CPU and leaves on another gets the right one back. A kernel
 * that gave programs their own GS or FS base (thread-local storage) would
 * have to save and restore it per task.
 *
 * The paths are: the `syscall` entry and its `sysretq` exit, interrupts and
 * exceptions that arrive from ring 3 (the stub checks the saved CS), and the
 * `iretq` exits to ring 3 (first entry of a program, fork child, sigreturn).
 */

#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gdt.h"

#define MAX_CPUS 32
#define LOCK_HELD_MAX 8

struct task;
struct spinlock;

// The first three fields are used from assembly (syscall_entry.S) by offset:
// do not move them.
typedef struct cpu {
    struct cpu *self;          // gs:[0]   This structure's own address
    uint64_t kernel_rsp;       // gs:[8]   Kernel stack for a system call arriving on this CPU
    uint64_t user_rsp;         // gs:[16]  Scratch: the user's RSP during the first instructions of a system call
    struct task *current;      // gs:[24]  Task running on this CPU
    struct task *idle;         // What this CPU runs when nothing else is runnable
    bool online;               // Takes part in scheduling
    volatile uint32_t resched_kicked;    // Another CPU has told this one there is work
    volatile uint32_t tlb_flush_pending; // Another CPU has changed the kernel's page tables

    // Spinlocks this CPU holds, in the order taken (see spinlock.h)
    struct spinlock *held[LOCK_HELD_MAX];
    uint32_t held_count;

    uint32_t cpu_id;           // Our numbering: 0 is the boot CPU
    uint32_t lapic_id;         // The hardware's identifier
    bool active;               // Started and reported in

    struct tss tss;            // This CPU's task state segment
    void *fault_stack;         // Its stack for double faults (TSS IST1)

    // Work another CPU has asked this one to do (smp_call)
    void (*call_fn)(void *);
    void *call_arg;
    volatile uint32_t call_pending;  // 1 from the request until the function has returned
    volatile uint64_t calls_done;

    // Set by the breakpoint exception handler: evidence that this CPU can
    // take exceptions on its own, and that it knows who it is while doing so
    uint64_t breakpoints;
    uint32_t breakpoint_lapic_id;
} cpu_t;

_Static_assert(offsetof(cpu_t, kernel_rsp) == 8, "syscall_entry.S uses gs:[8]");
_Static_assert(offsetof(cpu_t, user_rsp) == 16, "syscall_entry.S uses gs:[16]");
_Static_assert(offsetof(cpu_t, current) == 24, "cpu_current_task uses gs:[24]");

/**
 * @brief The structure of the CPU this code is running on.
 */
static inline cpu_t *this_cpu(void) {
    cpu_t *cpu;
    asm volatile("mov %0, gs:[0]" : "=r"(cpu));
    return cpu;
}

/**
 * @brief The task running on this CPU.
 *
 * One instruction, on purpose. A task can be moved to another CPU whenever
 * interrupts are on, so "find my CPU, then read its current task" could read
 * some other CPU's task if the move happened between the two steps.
 * this_cpu() has that problem in general: use its result only with
 * interrupts off.
 */
static inline struct task *cpu_current_task(void) {
    struct task *task;
    asm volatile("mov %0, gs:[24]" : "=r"(task));
    return task;
}

/**
 * @brief The structure for CPU number `cpu_id` (NULL if out of range).
 */
cpu_t *cpu_get(uint32_t cpu_id);

/**
 * @brief Make GS on the calling CPU point at its structure, and load its
 *        task state segment. Called once on each CPU, after the GDT is
 *        loaded there (loading segment registers resets the GS base).
 * @param fault_stack_top Top of a stack reserved for double faults.
 */
void cpu_init_this(uint32_t cpu_id, uint64_t fault_stack_top);

/**
 * @brief Read the GS base the CPU is using right now (for tests).
 */
uint64_t cpu_read_gs_base(void);

#endif // PERCPU_H
