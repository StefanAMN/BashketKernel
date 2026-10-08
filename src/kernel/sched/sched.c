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
#include "smp.h"
#include "lapic.h"

extern void ctx_switch(uint64_t **old_rsp_ptr, uint64_t *new_rsp, uint64_t *new_pml4_phys);
extern void task_start_trampoline(void);

// The task running on this CPU lives in the CPU's own structure. (Read with
// one instruction: see cpu_current_task.)
#define current_task (cpu_current_task())
static task_t *task_queue_head = NULL;
static task_t *task_queue_tail = NULL;
static uint32_t next_pid = 0;
static bool scheduler_active = false;

// One task list and one lock for all CPUs. Each CPU, when it schedules,
// takes the next runnable task from the list that is allowed to run on it,
// so work spreads over the CPUs by itself: an idle CPU takes whatever
// becomes runnable. (Simple rather than fast: with many CPUs the one lock
// would be fought over, and tasks do not stay on the CPU whose caches they
// have warmed.)
//
// The scheduler lock protects the task list and, in every task, the fields
// that decide whether and when it runs: state, wait_channel, wake_tick,
// slice_left, kill_pending, sig_pending, the signal handlers, detached,
// parent_pid and exit_status.
//
// It is held across a task switch: the task that gives up the CPU takes it,
// and the task that comes in releases it (in sched_yield or sched_sleep
// where that task itself once switched away, or in sched_task_started for a
// task running for the first time). That is what makes it safe for another
// CPU to wake, or free, a task the moment it sees the task's state: while
// the outgoing task is still on its stack, nobody else can look.
static spinlock_t sched_lock = SPINLOCK_RANKED("sched", LOCK_RANK_SCHED);

uint64_t sched_lock_irqsave(void) {
    return spinlock_acquire_irqsave(&sched_lock);
}

void sched_unlock_irqrestore(uint64_t flags) {
    spinlock_release_irqrestore(&sched_lock, flags);
}

// Tell a CPU that has nothing to do that there is now a task it could run.
// Without this it would only notice at its next timer tick. One is enough:
// if that CPU's next look finds more work than it can take, the others have
// their ticks. Caller holds the scheduler lock.
static void kick_idle_cpu(const task_t *t) {
    cpu_t *self = this_cpu();
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cpu_t *cpu = cpu_get(i);
        if (!cpu->online || cpu == self || cpu->current != cpu->idle) continue;
        if (t->affinity != TASK_ANY_CPU && t->affinity != (int32_t)i) continue;
        if (!cpu->resched_kicked) {
            cpu->resched_kicked = 1;
            lapic_send_ipi(cpu->lapic_id, IPI_VECTOR_WAKE);
        }
        return;
    }
}

// Caller holds the scheduler lock
static void enqueue_task(task_t *t) {
    t->next = NULL;
    if (task_queue_head == NULL) {
        task_queue_head = t;
        task_queue_tail = t;
    } else {
        task_queue_tail->next = t;
        task_queue_tail = t;
    }
    if (t->state == TASK_READY) kick_idle_cpu(t);
}

// What a CPU runs when no task is runnable: wait for an interrupt, then see
// whether it made one runnable. Interrupts are off around the check and
// "sti; hlt" is atomic (an interrupt can not slip in between the two), so a
// wakeup is never slept through.
void sched_idle_loop(void) {
    for (;;) {
        asm volatile("cli");
        sched_yield();
        asm volatile("sti\n\thlt");
    }
}

static task_t *task_alloc_with_pid(const char *name, void (*entry)(void), bool assign_pid);

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

    // This task is the boot sequence and then the loop that frees dead
    // tasks. It stays on the boot CPU, which is also where every device
    // interrupt arrives.
    main_task->affinity = 0;

    // The idle task is not on the task list and has no PID of its own: it
    // is never chosen by the round robin, only when that finds nothing
    task_t *idle = task_alloc_with_pid("idle", sched_idle_loop, false);
    if (!idle) panic("sched_init: no memory for the idle task");
    idle->pid = IDLE_PID;
    idle->priority = PRIO_LOW;
    idle->affinity = 0;

    uint64_t flags = sched_lock_irqsave();
    main_task->pid = next_pid++;
    enqueue_task(main_task);
    this_cpu()->current = main_task;
    this_cpu()->idle = idle;
    this_cpu()->online = true;
    scheduler_active = true;
    sched_unlock_irqrestore(flags);

    kprintf("Scheduler Init: Preemptive multitasking enabled (Main Kernel PID: %d)\n", main_task->pid);
}

task_t *sched_prepare_cpu(uint32_t cpu_id) {
    // The CPU's own boot stack becomes its idle task's stack, the way the
    // boot CPU's became PID 0's: there is no separate stack to allocate,
    // and nothing to set up on it. The first switch away saves its place.
    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    if (!idle) return NULL;
    memset(idle, 0, sizeof(task_t));
    idle->name[0] = 'i'; idle->name[1] = 'd'; idle->name[2] = 'l'; idle->name[3] = 'e'; idle->name[4] = '\0';
    idle->pid = IDLE_PID;
    idle->state = TASK_RUNNING;
    idle->pml4_phys = (uint64_t *)vmm_get_kernel_pml4_phys();
    idle->detached = true;
    idle->priority = PRIO_LOW;
    idle->affinity = (int32_t)cpu_id;
    idle->last_cpu = cpu_id;
    cpu_get(cpu_id)->idle = idle;
    return idle;
}

void sched_join_this_cpu(void) {
    uint64_t flags = sched_lock_irqsave();
    cpu_t *cpu = this_cpu();
    cpu->current = cpu->idle;
    cpu->online = true;
    sched_unlock_irqrestore(flags);
}

// First C code a new task runs (from task_start_trampoline): the switch that
// brought it here was made with the scheduler lock held, and it is this
// task's job to release it.
void sched_task_started(void) {
    spinlock_release(&sched_lock);
    asm volatile("sti");
}

task_t *task_alloc(const char *name, void (*entry)(void)) {
    return task_alloc_with_pid(name, entry, true);
}

static task_t *task_alloc_with_pid(const char *name, void (*entry)(void), bool assign_pid) {
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
    t->affinity = TASK_ANY_CPU;
    t->pml4_phys = (uint64_t *)vmm_get_kernel_pml4_phys(); // Kernel threads share the kernel address space
    t->detached = true;
    t->parent_pid = current_task ? current_task->pid : 0;
    t->priority = current_task ? current_task->priority : PRIO_NORMAL; // Inherited

    // Assign the PID now so the creator can use it before the task ever runs
    if (assign_pid) {
        uint64_t flags = sched_lock_irqsave();
        t->pid = next_pid++;
        sched_unlock_irqrestore(flags);
    }

    // Build initial stack frame for ctx_switch conforming to System V AMD64 ABI.
    // At function entry point, (RSP + 8) must be a multiple of 16 (i.e. RSP % 16 == 8).
    // With stack_raw 16-byte aligned, task_exit sits at (stack_raw - 8).
    // ctx_switch pops the callee-saved registers and returns into the
    // trampoline, which releases the scheduler lock and returns into entry
    // with RSP = stack_raw - 8, satisfying the alignment requirement.
    uint64_t stack_raw = (uint64_t)t->kernel_stack + KERNEL_STACK_SIZE;
    stack_raw &= ~0xFULL; // Align to 16-byte boundary
    uint64_t *stack_top = (uint64_t *)stack_raw;

    *(--stack_top) = (uint64_t)task_exit;  // Return address if entry returns
    *(--stack_top) = (uint64_t)entry;      // Target entry point (popped by the trampoline's ret)
    *(--stack_top) = (uint64_t)task_start_trampoline; // Popped by ret in ctx_switch
    *(--stack_top) = 0x002;                // RFLAGS: interrupts off, the scheduler lock is held; the trampoline enables them
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
    uint64_t flags = sched_lock_irqsave();
    enqueue_task(t);
    sched_unlock_irqrestore(flags);
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
    vfs_lock();
    if (dir) dir->open_count++;
    if (task->cwd) task->cwd->open_count--;
    task->cwd = dir;
    vfs_unlock();
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

void task_set_affinity(task_t *task, int32_t cpu_id) {
    if (!task) return;
    uint64_t flags = sched_lock_irqsave();
    cpu_t *cpu = cpu_id >= 0 ? cpu_get((uint32_t)cpu_id) : NULL;
    // A task tied to a CPU that is not scheduling would never run again
    task->affinity = (cpu && cpu->online) ? cpu_id : TASK_ANY_CPU;
    sched_unlock_irqrestore(flags);
}

uint32_t sched_running_count(void) {
    uint32_t running = 0;
    uint64_t flags = sched_lock_irqsave();
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->state == TASK_RUNNING) running++;
    }
    sched_unlock_irqrestore(flags);
    return running;
}

task_t *sched_first_task(void) {
    return task_queue_head;
}

task_t *sched_find_task_locked(uint32_t pid) {
    task_t *curr = task_queue_head;
    while (curr && curr->pid != pid) {
        curr = curr->next;
    }
    return curr;
}

task_t *sched_find_task(uint32_t pid) {
    uint64_t flags = sched_lock_irqsave();
    task_t *task = sched_find_task_locked(pid);
    sched_unlock_irqrestore(flags);
    return task;
}

task_t *sched_get_current_task(void) {
    return current_task;
}

// Give the CPU to another task. The caller holds the scheduler lock and has
// already set the current task's state if it is not to stay runnable. Returns
// when this task is next chosen to run, still (again) holding the lock.
static void schedule(void) {
    cpu_t *cpu = this_cpu();
    task_t *old_task = cpu->current;
    task_t *next_task = NULL;

    // Rule 4 of spinlock.h: a task that switches away holding some other
    // lock would leave it locked for as long as it sleeps
    if (lock_debug_held_count() > 1) {
        lock_debug_report("task switch while holding", cpu->held[0]->name, NULL);
    }

    // Round robin: walk the list once, starting after the current task and
    // wrapping at the end, and take the first runnable task. (The idle task
    // is not on the list: from there, start at the head.)
    // A task that is READY is on no CPU: whoever last ran it finished
    // switching away before releasing this lock.
    cpu->resched_kicked = 0;
    int32_t here = (int32_t)cpu->cpu_id;
    task_t *start = (old_task != cpu->idle && old_task->next) ? old_task->next : task_queue_head;
    for (task_t *t = start; t && !next_task; t = t->next) {
        if (t->state == TASK_READY && (t->affinity == TASK_ANY_CPU || t->affinity == here)) next_task = t;
    }
    for (task_t *t = task_queue_head; t != start && !next_task; t = t->next) {
        if (t->state == TASK_READY && (t->affinity == TASK_ANY_CPU || t->affinity == here)) next_task = t;
    }

    // The current task may only carry on here if it is still allowed on
    // this CPU (its affinity may have been changed while it ran)
    bool may_stay = old_task->state == TASK_RUNNING &&
                    (old_task->affinity == TASK_ANY_CPU || old_task->affinity == here);

    if (!next_task) {
        if (may_stay) return;       // Nobody else wants the CPU: carry on
        next_task = cpu->idle;      // We can not carry on: idle until someone can
    }

    if (old_task->state == TASK_RUNNING) {
        old_task->state = TASK_READY;
        if (!may_stay) kick_idle_cpu(old_task); // Its CPU may be asleep
    }
    next_task->state = TASK_RUNNING;
    next_task->last_cpu = cpu->cpu_id;
    next_task->slice_left = priority_time_slice(next_task->priority); // A fresh turn
    cpu->current = next_task;

    // Point the CPU at the incoming task's kernel stack for the next entry
    // from ring 3: TSS.RSP0 for interrupts and exceptions, this CPU's kernel_rsp
    // for the syscall instruction (which does not use the TSS).
    if (next_task->kernel_stack) {
        uint64_t kstack_top = (uint64_t)next_task->kernel_stack + KERNEL_STACK_SIZE;
        tss_set_rsp0(kstack_top);
        cpu->kernel_rsp = kstack_top;
    }

    // Floating point registers are not part of ctx_switch. The kernel never
    // uses them, so only user tasks have any to keep.
    if (old_task->is_user) fpu_save(old_task->fpu_state);
    if (next_task->is_user) fpu_restore(next_task->fpu_state);

    ctx_switch((uint64_t **)&old_task->rsp, (uint64_t *)next_task->rsp, next_task->pml4_phys);

    // Back in old_task, possibly much later and on a different CPU (`cpu`
    // above is no longer ours), with the lock held by whoever switched to us
    // and interrupts off.
}

void sched_yield(void) {
    if (!scheduler_active || !current_task) return;

    uint64_t flags = sched_lock_irqsave();
    schedule();
    sched_unlock_irqrestore(flags);
}

// --- Blocking ----------------------------------------------------------------
//
// A task that has to wait for something (a key press, a child exiting, a point
// in time) marks itself TASK_BLOCKED and names a "channel": any address that
// identifies the event. The scheduler skips blocked tasks, so waiting costs no
// CPU time. Whoever makes the event happen calls sched_wakeup(channel).
//
// The classic bug here is the lost wakeup: the task checks the condition,
// someone else (an interrupt handler, or a task on another CPU) makes the
// condition true and calls wakeup, and only then does the task go to sleep,
// forever. The rule that prevents it: the condition is protected by a lock,
// which the sleeper holds while it checks and hands to sched_sleep:
//
//     uint64_t flags = spinlock_acquire_irqsave(&lock);
//     while (!condition) sched_sleep(channel, &lock, 0);
//     spinlock_release_irqrestore(&lock, flags);
//
// and which the waker holds while it changes the condition. sched_sleep
// marks the task blocked under the scheduler lock *before* it releases the
// caller's lock, so the waker can not get in between.

// Caller holds the scheduler lock
static void wake_task(task_t *t) {
    t->wait_channel = NULL;
    t->wake_tick = 0;
    t->state = TASK_READY;
    kick_idle_cpu(t);
}

static void sleep_on(void *channel, spinlock_t *lock, uint64_t wake_tick, bool interruptible) {
    task_t *self = current_task;
    if (!scheduler_active || !self) return;

    // The caller's lock may be the scheduler lock itself (process_wait
    // sleeps on the task list)
    bool caller_has_sched_lock = (lock == &sched_lock);
    uint64_t flags = 0;
    if (!caller_has_sched_lock) flags = sched_lock_irqsave();

    if (interruptible) {
        // A process that has been killed must not go (back) to sleep.
        // Anything that sleeps interruptibly is at a point where it holds
        // nothing but `lock` and is prepared to lose the CPU, so this is a
        // safe place to end it.
        if (self->kill_pending) {
            int sig = self->kill_pending;
            spinlock_release(&sched_lock);
            if (lock && !caller_has_sched_lock) spinlock_release(lock);
            task_exit_status(128 + sig);
        }

        // A signal with a handler is waiting: do not sleep on it. The caller
        // sees sched_interrupted() and abandons its wait with EINTR.
        if (self->sig_pending) {
            if (!caller_has_sched_lock) sched_unlock_irqrestore(flags);
            return;
        }
    }

    self->wait_channel = channel;
    self->wake_tick = wake_tick;
    self->state = TASK_BLOCKED;

    if (lock && !caller_has_sched_lock) spinlock_release(lock);
    schedule(); // Returns once someone has woken us and we have been chosen to run

    int die = (interruptible && self->kill_pending) ? self->kill_pending : 0; // Woken to die
    spinlock_release(&sched_lock);
    if (die) task_exit_status(128 + die);

    // Take the caller's lock back, in the right order (it ranks below the
    // scheduler lock), and put interrupts back as the caller had them
    if (caller_has_sched_lock) {
        spinlock_acquire(&sched_lock);
    } else {
        if (lock) spinlock_acquire(lock);
        irq_restore(flags);
    }
}

void sched_sleep(void *channel, spinlock_t *lock, uint64_t wake_tick) {
    sleep_on(channel, lock, wake_tick, true);
}

void sched_sleep_uninterruptible(void *channel, spinlock_t *lock, uint64_t wake_tick) {
    sleep_on(channel, lock, wake_tick, false);
}

void sched_sleep_on_task_list(void *channel) {
    sleep_on(channel, &sched_lock, 0, true);
}

bool sched_interrupted(void) {
    return current_task && current_task->sig_pending != 0;
}

void sched_interrupt_sleep(task_t *task) {
    if (task->state == TASK_BLOCKED) wake_task(task);
}

void sched_request_kill(task_t *task, int sig) {
    if (task->state != TASK_ZOMBIE && !task->exiting && task->kill_pending == 0) {
        task->kill_pending = sig;
        // A sleeper would otherwise not notice until its event happens
        if (task->state == TASK_BLOCKED) wake_task(task);
    }
}

void sched_check_kill(void) {
    task_t *self = current_task;
    if (self && self->kill_pending) task_exit_status(128 + self->kill_pending);
}

// Caller holds the scheduler lock
static void wakeup_locked(void *channel) {
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->state == TASK_BLOCKED && t->wait_channel == channel) {
            wake_task(t);
        }
    }
}

void sched_wakeup(void *channel) {
    uint64_t flags = sched_lock_irqsave();
    wakeup_locked(channel);
    sched_unlock_irqrestore(flags);
}

void sched_wake_sleepers(uint64_t now) {
    uint64_t flags = sched_lock_irqsave();
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->state == TASK_BLOCKED && t->wake_tick != 0 && now >= t->wake_tick) {
            wake_task(t);
        }
    }
    sched_unlock_irqrestore(flags);
}

void sched_unlink_zombie(task_t *task) {
    task_t *prev = NULL;
    for (task_t *curr = task_queue_head; curr; prev = curr, curr = curr->next) {
        if (curr != task) continue;
        if (prev) prev->next = curr->next; else task_queue_head = curr->next;
        if (curr == task_queue_tail) task_queue_tail = prev;
        break;
    }
    task->next = NULL;
}

size_t sched_reap(void) {
    task_t *dead_list = NULL;
    size_t reaped = 0;

    // Unlink zombies under the lock, free them afterwards. A zombie can not
    // free itself: it is still running on the stack that would be freed.
    // Seeing ZOMBIE under the lock means it has left that stack for good
    // (see the comment on sched_lock).
    uint64_t flags = sched_lock_irqsave();
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
    sched_unlock_irqrestore(flags);

    while (dead_list) {
        task_t *next = dead_list->next;
        task_free(dead_list);
        dead_list = next;
        reaped++;
    }

    return reaped;
}

void sched_tick(void) {
    // Another CPU has stopped the kernel: stop here too
    if (kernel_halted) for (;;) asm volatile("cli\n\thlt");

    if (!scheduler_active || !current_task) return;

    uint64_t flags = sched_lock_irqsave();
    task_t *self = current_task;
    self->cpu_ticks++;

    // Preempt only when the running task has used up its turn. The idle
    // task has no turn: it gives way as soon as anything is runnable.
    if (self != this_cpu()->idle && self->state == TASK_RUNNING && self->slice_left > 1) {
        self->slice_left--;
    } else {
        schedule();
    }
    sched_unlock_irqrestore(flags);
}

void task_exit(void) {
    task_exit_status(0);
}

void task_exit_status(int status) {
    task_t *self = current_task;
    if (!self) return;

    // From here on the task can not be killed again, and nothing it does
    // while cleaning up is cut short by a signal
    uint64_t flags = sched_lock_irqsave();
    self->exiting = true;
    self->kill_pending = 0;
    self->sig_pending = 0;
    sched_unlock_irqrestore(flags);

    // Close its files now rather than when it is reaped: a reader at the
    // other end of a pipe must see end-of-file as soon as the writer dies.
    // (Done before taking the scheduler lock: closing takes other locks
    // and may sleep.)
    file_close_all(self->fds);
    task_set_cwd(self, NULL);
    tty_process_exited(self->pid);

    sched_lock_irqsave(); // Never released by this task: see sched_lock
    self->exit_status = status;
    self->state = TASK_ZOMBIE;

    // Children of a dead task have nobody left to wait for them
    task_t *parent = NULL;
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->parent_pid == self->pid && t != self) t->detached = true;
        if (t->pid == self->parent_pid && t != self) parent = t;
    }

    // A parent blocked in process_wait sleeps on its own task struct
    if (parent) wakeup_locked(parent);

    // A zombie is never chosen again. Whoever sees its state from now on
    // may free it, stack included, as soon as this switch is complete.
    schedule();
    panic("a zombie task was scheduled");
}

void sched_print_tasks(void) {
    kprintf("PID\tSTATE\t\tNAME\n");
    kprintf("----------------------------------------\n");
    uint64_t flags = sched_lock_irqsave();
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
    sched_unlock_irqrestore(flags);
}
