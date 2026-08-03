/**
 * @file sched.c
 * @brief Implementation of Round-Robin Preemptive Multitasking Scheduler
 */

#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "kprintf.h"
#include "string_util.h"
#include "kstack.h"
#include "spinlock.h"
#include "gdt.h"
#include "file.h"
#include "fpu.h"
#include "priority.h"
#include "tty.h"
#include "percpu.h"

extern void ctx_switch(uint64_t **old_rsp_ptr, uint64_t *new_rsp, uint64_t *new_pml4_phys);

// The task running on this CPU lives in the CPU's own structure
#define current_task (this_cpu()->current)
static task_t *task_queue_head = NULL;
static task_t *task_queue_tail = NULL;
static uint32_t next_pid = 0;
static bool scheduler_active = false;

// The run queue is touched from task context and from the timer interrupt.
// On one CPU, disabling interrupts is enough to make each update atomic;
// real SMP will need a lock here as well.
static void enqueue_task(task_t *t) {
    uint64_t flags = irq_save();
    t->next = NULL;
    if (task_queue_head == NULL) {
        task_queue_head = t;
        task_queue_tail = t;
    } else {
        task_queue_tail->next = t;
        task_queue_tail = t;
    }
    irq_restore(flags);
}

void sched_init(void) {
    // Create PID 0 representation for the main running kernel execution context
    task_t *main_task = (task_t *)kmalloc(sizeof(task_t));
    memset(main_task, 0, sizeof(task_t));
    main_task->name[0] = 'k'; main_task->name[1] = 'e'; main_task->name[2] = 'r'; main_task->name[3] = 'n';
    main_task->name[4] = 'e'; main_task->name[5] = 'l'; main_task->name[6] = '_'; main_task->name[7] = 'm';
    main_task->name[8] = 'a'; main_task->name[9] = 'i'; main_task->name[10] = 'n'; main_task->name[11] = '\0';
    main_task->state = TASK_RUNNING;
    main_task->pml4_phys = (uint64_t *)vmm_get_kernel_pml4_phys();
    main_task->detached = true;
    main_task->priority = PRIO_NORMAL;
    main_task->slice_left = priority_time_slice(PRIO_NORMAL);
    main_task->next = NULL;

    main_task->pid = next_pid++;
    enqueue_task(main_task);
    current_task = main_task;

    scheduler_active = true;
    kprintf("Scheduler Init: Preemptive multitasking enabled (Main Kernel PID: %d)\n", main_task->pid);
}

task_t *task_alloc(const char *name, void (*entry)(void)) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(task_t));

    if (name) {
        size_t i = 0;
        for (; name[i] != '\0' && i < sizeof(t->name) - 1; i++) {
            t->name[i] = name[i];
        }
        t->name[i] = '\0';
    } else {
        t->name[0] = 't'; t->name[1] = 'a'; t->name[2] = 's'; t->name[3] = 'k'; t->name[4] = '\0';
    }

    // Allocate a 16 KiB kernel stack with an unmapped guard page below it
    t->kernel_stack = kstack_alloc();
    if (!t->kernel_stack) {
        kfree(t);
        return NULL;
    }

    t->state = TASK_READY;
    t->pml4_phys = (uint64_t *)vmm_get_kernel_pml4_phys(); // Kernel threads share the kernel address space
    t->detached = true;
    t->parent_pid = current_task ? current_task->pid : 0;
    t->priority = current_task ? current_task->priority : PRIO_NORMAL; // Inherited

    // Assign the PID now so the creator can use it before the task ever runs
    uint64_t flags = irq_save();
    t->pid = next_pid++;
    irq_restore(flags);

    // Build initial stack frame for ctx_switch conforming to System V AMD64 ABI.
    // At function entry point, (RSP + 8) must be a multiple of 16 (i.e. RSP % 16 == 8).
    // With stack_raw 16-byte aligned, task_exit sits at (stack_raw - 8).
    // When ctx_switch pops callee-saved registers and ret pops entry, RSP becomes
    // stack_raw - 8, perfectly satisfying the 16-byte alignment requirement.
    uint64_t stack_raw = (uint64_t)t->kernel_stack + KERNEL_STACK_SIZE;
    stack_raw &= ~0xFULL; // Align to 16-byte boundary
    uint64_t *stack_top = (uint64_t *)stack_raw;

    *(--stack_top) = (uint64_t)task_exit;  // Return address if entry returns
    *(--stack_top) = (uint64_t)entry;      // Target entry point (popped by ret in ctx_switch)
    *(--stack_top) = 0x202;                // RFLAGS (Interrupts enabled 0x200 | bit 1 set)
    *(--stack_top) = 0;                    // R15
    *(--stack_top) = 0;                    // R14
    *(--stack_top) = 0;                    // R13
    *(--stack_top) = 0;                    // R12
    *(--stack_top) = 0;                    // RBP
    *(--stack_top) = 0;                    // RBX

    t->rsp = (uint64_t)stack_top;
    return t;
}

void sched_add(task_t *t) {
    enqueue_task(t);
}

task_t *task_create(const char *name, void (*entry)(void)) {
    task_t *t = task_alloc(name, entry);
    if (!t) return NULL;

    kprintf("Task Created: PID %d ('%s') at Entry: %p\n", t->pid, t->name, entry);
    sched_add(t);
    return t;
}

void task_set_cwd(task_t *task, vnode_t *dir) {
    // A directory in use as someone's working directory counts as open, so
    // it can not be removed from under them
    uint64_t flags = irq_save();
    if (dir) dir->open_count++;
    if (task->cwd) task->cwd->open_count--;
    task->cwd = dir;
    irq_restore(flags);
}

void task_free(task_t *t) {
    if (!t) return;
    file_close_all(t->fds); // Normally already empty: closed when it died
    task_set_cwd(t, NULL);
    if (t->is_user && t->pml4_phys && (uint64_t)t->pml4_phys != vmm_get_kernel_pml4_phys()) {
        vmm_destroy_address_space(vmm_space_from_phys((uint64_t)t->pml4_phys));
    }
    if (t->kernel_stack) {
        kstack_free(t->kernel_stack);
    }
    kfree(t);
}

task_t *sched_first_task(void) {
    return task_queue_head;
}

task_t *sched_find_task(uint32_t pid) {
    uint64_t flags = irq_save();
    task_t *curr = task_queue_head;
    while (curr && curr->pid != pid) {
        curr = curr->next;
    }
    irq_restore(flags);
    return curr;
}

task_t *sched_get_current_task(void) {
    return current_task;
}

void sched_yield(void) {
    if (!scheduler_active || !current_task) return;

    // Interrupts stay off from picking the next task until the switch is done.
    // Otherwise a timer tick in between would re-enter this function with
    // current_task already changed but the stack not yet switched.
    uint64_t flags = irq_save();

    task_t *old_task = current_task;
    task_t *next_task = NULL;

    // Round robin: walk the queue once, starting after the current task and
    // wrapping at the end, and take the first runnable task. The walk ends
    // when it comes back to the current task, whatever state that is in.
    task_t *curr = old_task;
    do {
        curr = curr->next ? curr->next : task_queue_head;
        if (curr != old_task && curr->state == TASK_READY) {
            next_task = curr;
            break;
        }
    } while (curr != old_task);

    if (!next_task) {
        irq_restore(flags);
        return; // No other task ready to run
    }

    if (old_task->state == TASK_RUNNING) {
        old_task->state = TASK_READY;
    }
    next_task->state = TASK_RUNNING;
    next_task->slice_left = priority_time_slice(next_task->priority); // A fresh turn
    current_task = next_task;

    // Point the CPU at the incoming task's kernel stack for the next entry
    // from ring 3: TSS.RSP0 for interrupts and exceptions, this CPU's kernel_rsp
    // for the syscall instruction (which does not use the TSS).
    if (next_task->kernel_stack) {
        uint64_t kstack_top = (uint64_t)next_task->kernel_stack + KERNEL_STACK_SIZE;
        tss_set_rsp0(kstack_top);
        this_cpu()->kernel_rsp = kstack_top;
    }

    // Floating point registers are not part of ctx_switch. The kernel never
    // uses them, so only user tasks have any to keep.
    if (old_task->is_user) fpu_save(old_task->fpu_state);
    if (next_task->is_user) fpu_restore(next_task->fpu_state);

    ctx_switch((uint64_t **)&old_task->rsp, (uint64_t *)next_task->rsp, next_task->pml4_phys);

    // Back in old_task, possibly much later. ctx_switch restored the RFLAGS it
    // saved (interrupts off); now restore what the caller had.
    irq_restore(flags);
}

// --- Blocking ----------------------------------------------------------------
//
// A task that has to wait for something (a key press, a child exiting, a point
// in time) marks itself TASK_BLOCKED and names a "channel": any address that
// identifies the event. The scheduler skips blocked tasks, so waiting costs no
// CPU time. Whoever makes the event happen calls sched_wakeup(channel).
//
// The classic bug here is the lost wakeup: the task checks the condition, an
// interrupt makes the condition true and calls wakeup, and only then does the
// task go to sleep, forever. The rule that prevents it: check the condition and
// call sched_block with interrupts disabled, in a loop:
//
//     uint64_t flags = irq_save();
//     while (!condition) sched_block(channel, 0);
//     irq_restore(flags);

static void wake_task(task_t *t) {
    t->wait_channel = NULL;
    t->wake_tick = 0;
    t->state = TASK_READY;
}

static void block_current(void *channel, uint64_t wake_tick, bool interruptible) {
    task_t *self = current_task;
    if (!scheduler_active || !self) return;

    uint64_t flags = irq_save();

    if (interruptible) {
        // A process that has been killed must not go (back) to sleep.
        // Anything that sleeps interruptibly is at a point where it holds
        // nothing and is prepared to lose the CPU, so this is a safe place
        // to end it.
        if (self->kill_pending) task_exit_status(128 + self->kill_pending);

        // A signal with a handler is waiting: do not sleep on it. The caller
        // sees sched_interrupted() and abandons its wait with EINTR.
        if (self->sig_pending) {
            irq_restore(flags);
            return;
        }
    }

    self->wait_channel = channel;
    self->wake_tick = wake_tick;
    self->state = TASK_BLOCKED;

    while (self->state == TASK_BLOCKED) {
        sched_yield();
        if (self->state == TASK_BLOCKED) {
            // Nothing else is runnable. Wait for an interrupt. "sti; hlt" is
            // atomic: an interrupt can not slip in between the two.
            asm volatile("sti\n\thlt\n\tcli");
        }
    }

    // If the wakeup arrived while we were halted above, no switch happened and
    // the state still says READY.
    self->state = TASK_RUNNING;

    if (interruptible && self->kill_pending) task_exit_status(128 + self->kill_pending); // Woken to die
    irq_restore(flags);
}

void sched_block(void *channel, uint64_t wake_tick) {
    block_current(channel, wake_tick, true);
}

void sched_block_uninterruptible(void *channel, uint64_t wake_tick) {
    block_current(channel, wake_tick, false);
}

bool sched_interrupted(void) {
    return current_task && current_task->sig_pending != 0;
}

void sched_interrupt_sleep(task_t *task) {
    uint64_t flags = irq_save();
    if (task->state == TASK_BLOCKED) wake_task(task);
    irq_restore(flags);
}

void sched_request_kill(task_t *task, int sig) {
    uint64_t flags = irq_save();
    if (task->state != TASK_ZOMBIE && task->kill_pending == 0) {
        task->kill_pending = sig;
        // A sleeper would otherwise not notice until its event happens
        if (task->state == TASK_BLOCKED) wake_task(task);
    }
    irq_restore(flags);
}

void sched_check_kill(void) {
    task_t *self = current_task;
    if (self && self->kill_pending) task_exit_status(128 + self->kill_pending);
}

void sched_wakeup(void *channel) {
    uint64_t flags = irq_save();
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->state == TASK_BLOCKED && t->wait_channel == channel) {
            wake_task(t);
        }
    }
    irq_restore(flags);
}

void sched_wake_sleepers(uint64_t now) {
    uint64_t flags = irq_save();
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->state == TASK_BLOCKED && t->wake_tick != 0 && now >= t->wake_tick) {
            wake_task(t);
        }
    }
    irq_restore(flags);
}

void sched_make_zombie(task_t *task, int status) {
    uint64_t flags = irq_save();
    task->exit_status = status;
    task->state = TASK_ZOMBIE;

    // Close its files now rather than when it is reaped: a reader at the
    // other end of a pipe must see end-of-file as soon as the writer dies.
    file_close_all(task->fds);
    task_set_cwd(task, NULL);
    tty_process_exited(task->pid);

    // Children of a dead task have nobody left to wait for them
    task_t *parent = NULL;
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->parent_pid == task->pid && t != task) t->detached = true;
        if (t->pid == task->parent_pid && t != task) parent = t;
    }

    // A parent blocked in process_wait sleeps on its own task struct
    if (parent) sched_wakeup(parent);
    irq_restore(flags);
}

void sched_free_zombie(task_t *task) {
    if (!task || task == current_task || task->state != TASK_ZOMBIE) return;

    uint64_t flags = irq_save();
    task_t *prev = NULL;
    for (task_t *curr = task_queue_head; curr; prev = curr, curr = curr->next) {
        if (curr != task) continue;
        if (prev) prev->next = curr->next; else task_queue_head = curr->next;
        if (curr == task_queue_tail) task_queue_tail = prev;
        break;
    }
    irq_restore(flags);

    task_free(task);
}

size_t sched_reap(void) {
    task_t *dead_list = NULL;
    size_t reaped = 0;

    // Unlink zombies with interrupts off, free them afterwards. A zombie can
    // not free itself: it is still running on the stack that would be freed.
    uint64_t flags = irq_save();
    task_t *prev = NULL;
    task_t *curr = task_queue_head;
    while (curr) {
        task_t *next = curr->next;
        if (curr->state == TASK_ZOMBIE && curr->detached && curr != current_task) {
            if (prev) prev->next = next; else task_queue_head = next;
            if (curr == task_queue_tail) task_queue_tail = prev;
            curr->next = dead_list;
            dead_list = curr;
        } else {
            prev = curr;
        }
        curr = next;
    }
    irq_restore(flags);

    while (dead_list) {
        task_t *next = dead_list->next;
        task_free(dead_list);
        dead_list = next;
        reaped++;
    }

    return reaped;
}

void sched_tick(void) {
    if (!scheduler_active || !current_task) return;

    current_task->cpu_ticks++;

    // Preempt only when the running task has used up its turn. A task that
    // is no longer runnable (it is on its way to sleep) gives way at once.
    if (current_task->state == TASK_RUNNING && current_task->slice_left > 1) {
        current_task->slice_left--;
        return;
    }
    sched_yield();
}

void task_exit(void) {
    task_exit_status(0);
}

void task_exit_status(int status) {
    if (!current_task) return;

    sched_make_zombie(current_task, status);

    // A zombie is never picked again, so this only returns if no other task
    // is runnable right now. Keep yielding until sched_reap frees us.
    while (1) {
        sched_yield();
        asm volatile("sti\n\thlt"); // IF may be clear if we got here from a fault handler
    }
}

void sched_print_tasks(void) {
    kprintf("PID\tSTATE\t\tNAME\n");
    kprintf("----------------------------------------\n");
    task_t *curr = task_queue_head;
    while (curr) {
        const char *state_str = "UNKNOWN";
        switch (curr->state) {
            case TASK_READY:   state_str = "READY  "; break;
            case TASK_RUNNING: state_str = "RUNNING"; break;
            case TASK_BLOCKED: state_str = "BLOCKED"; break;
            case TASK_ZOMBIE:  state_str = "ZOMBIE "; break;
        }
        kprintf("%d\t%s\t%s\n", curr->pid, state_str, curr->name);
        curr = curr->next;
    }
}
