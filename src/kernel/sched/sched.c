/**
 * @file sched.c
 * @brief Implementation of Round-Robin Preemptive Multitasking Scheduler
 */

#include "sched.h"
#include "region.h"
#include "pcache.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "kprintf.h"
#include "kstring.h"
#include "kstack.h"
#include "spinlock.h"
#include "gdt.h"
#include "file.h"
#include "fpu.h"
#include "priority.h"
#include "tty.h"
#include "percpu.h"
#include "signal.h"
#include "process.h"
#include "smp.h"
#include "lapic.h"
#include "timer.h"

extern void ctx_switch(uint64_t **old_rsp_ptr, uint64_t *new_rsp, uint64_t *new_pml4_phys);
extern void task_start_trampoline(void);

// The task running on this CPU lives in the CPU's own structure. (Read with
// one instruction: see cpu_current_task.)
#define current_task (cpu_current_task())
static task_t *task_queue_head = NULL;
static task_t *task_queue_tail = NULL;
static uint32_t next_pid = 0;
static bool scheduler_active = false;

// The process that inherits children whose parent has died, and waits for
// them (see sched_set_init). NULL: there is none, and orphans are detached.
static task_t *init_task = NULL;

// One task list, and one queue of runnable tasks per CPU.
//
// The scheduler lock protects the task list and, in every task, the fields
// that say whether it may run and why not: state (as far as BLOCKED,
// STOPPED and ZOMBIE go), wait_channel, wake_tick, kill_pending,
// sig_pending, the signal handlers, detached, parent_pid and exit_status.
// Sleeping, waking, signals, parents and reaping all go through it.
//
// Who runs next is each CPU's own business: a run queue holds the READY
// tasks waiting for that CPU, under a lock of its own (ranked just above the
// scheduler lock: taken under it, never the other way round). The timer
// tick, yield and the idle loop take only their CPU's queue lock. A CPU
// whose queue is empty takes a task from the CPU with the most waiting
// (steal), with a try-lock: it holds its own queue's lock meanwhile, and two
// CPUs taking from each other must not wait for each other. A task woken
// or created goes to the CPU it last ran on unless that one has more than
// one task more than the least loaded (choose_cpu); a task bound to a CPU
// (affinity) only ever goes to that CPU's queue.
//
// Held across a task switch: the outgoing task holds its CPU's queue lock
// through ctx_switch, and the task that comes in releases it (finish_switch,
// after ctx_switch or in sched_task_started for a task's first run). So a
// task pushed to the back of a queue as it gives up the CPU cannot be
// taken by another CPU before it has left its stack. A task that sleeps,
// stops or exits holds the scheduler lock across the switch as well (the
// queue's release_sched note tells the incoming task to release that too):
// whoever sees it BLOCKED, STOPPED or ZOMBIE under the scheduler lock sees
// a task that has left its stack. That is what lets another CPU wake it, or
// free a zombie's stack, the moment it sees the state.
static spinlock_t sched_lock = SPINLOCK_RANKED("sched", LOCK_RANK_SCHED);

typedef struct {
    spinlock_t lock;
    task_t *head, *tail;       // READY tasks waiting for this CPU, oldest first (task->run_next)
    volatile uint32_t count;   // How many (read without the lock to compare loads)
    bool release_sched;        // The task switching away holds the scheduler lock too
    task_t *moving;            // A task switched away from here that may no longer run here: placed after the switch
} runqueue_t;

static runqueue_t runqueues[MAX_CPUS];

uint64_t sched_lock_irqsave(void) {
    return spinlock_acquire_irqsave(&sched_lock);
}

void sched_unlock_irqrestore(uint64_t flags) {
    spinlock_release_irqrestore(&sched_lock, flags);
}

uint64_t sched_lock_contention(void) {
    return __atomic_load_n(&sched_lock.contended, __ATOMIC_RELAXED);
}

uint32_t sched_queue_length(uint32_t cpu_id) {
    return cpu_id < MAX_CPUS ? runqueues[cpu_id].count : 0;
}

// Add `t` at the back of CPU `cpu`'s queue. Queue lock held.
static void rq_push(uint32_t cpu, task_t *t) {
    runqueue_t *rq = &runqueues[cpu];
    t->run_next = NULL;
    if (rq->tail) rq->tail->run_next = t; else rq->head = t;
    rq->tail = t;
    rq->count++;
    t->queued_on = (int32_t)cpu;
}

// Take `t` out of `rq` (it is in it; `prev` is the task before it, or
// NULL). Queue lock held.
static void rq_unlink(runqueue_t *rq, task_t *prev, task_t *t) {
    if (prev) prev->run_next = t->run_next; else rq->head = t->run_next;
    if (rq->tail == t) rq->tail = prev;
    rq->count--;
    t->run_next = NULL;
    t->queued_on = -1;
}

// The oldest task in `rq` (with `movable_only`, the oldest not bound to a
// CPU), taken out of it; NULL if there is none. Queue lock held.
static task_t *rq_take(runqueue_t *rq, bool movable_only) {
    task_t *prev = NULL;
    for (task_t *t = rq->head; t; prev = t, t = t->run_next) {
        if (movable_only && t->affinity != TASK_ANY_CPU) continue;
        rq_unlink(rq, prev, t);
        return t;
    }
    return NULL;
}

// How busy CPU `i` is: the tasks waiting for it and the one it runs (read
// without locks: a placement decision, which a stale value only makes a
// little worse)
static uint32_t load_of(uint32_t i) {
    cpu_t *cpu = cpu_get(i);
    return runqueues[i].count + (cpu->current != cpu->idle ? 1 : 0);
}

// Send the wake-up interrupt to CPU `i` if it is idle (and is not this
// one): it will look at its queue, or take work from another's. One
// interrupt per idle spell is enough (resched_kicked).
static void kick_cpu(uint32_t i) {
    cpu_t *cpu = cpu_get(i);
    if (!cpu->online || cpu == this_cpu() || cpu->current != cpu->idle || cpu->resched_kicked) return;
    cpu->resched_kicked = 1;
    lapic_send_ipi(cpu->lapic_id, IPI_VECTOR_WAKE);
}

// Wake one idle CPU (not this one), if there is one, to take a task that
// waits in a busy CPU's queue
static void kick_an_idle_cpu(void) {
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cpu_t *cpu = cpu_get(i);
        if (cpu->online && cpu != this_cpu() && cpu->current == cpu->idle) {
            kick_cpu(i);
            return;
        }
    }
}

// The CPU whose queue a runnable task should join: its own if it is bound;
// otherwise, if it has run before (`was_running`), the one it last ran on,
// unless that one has more than one task more than the least loaded,
// which it then goes to (as does a task that has never run)
static uint32_t choose_cpu(const task_t *t, bool was_running) {
    if (t->affinity != TASK_ANY_CPU) return (uint32_t)t->affinity;
    uint32_t least = 0, least_load = UINT32_MAX;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (!cpu_get(i)->online) continue;
        uint32_t load = load_of(i);
        if (load < least_load) {
            least = i;
            least_load = load;
        }
    }
    uint32_t last = t->last_cpu;
    if (!was_running || last >= MAX_CPUS || !cpu_get(last)->online) return least;
    return load_of(last) > least_load + 1 ? least : last;
}

// Make `t` READY, put it in the queue of the CPU it should run on, and wake
// that CPU if it is idle (or, if it is busy, an idle one that may take the
// task). `t` is on no CPU and in no queue. The caller holds the scheduler
// lock (or `t` is not yet known to anyone, or was just switched away from:
// finish_switch), and no queue lock.
static void place_task(task_t *t, bool was_running) {
    uint32_t cpu = choose_cpu(t, was_running);
    runqueue_t *rq = &runqueues[cpu];
    spinlock_acquire(&rq->lock);
    t->state = TASK_READY;
    rq_push(cpu, t);
    spinlock_release(&rq->lock);
    cpu_t *target = cpu_get(cpu);
    if (target->current == target->idle) kick_cpu(cpu);
    else if (t->affinity == TASK_ANY_CPU) kick_an_idle_cpu();
}

// Put `t` on the list of all tasks and, if it is READY, in a run queue.
// Caller holds the scheduler lock.
static void enqueue_task(task_t *t) {
    t->next = NULL;
    if (task_queue_head == NULL) {
        task_queue_head = t;
        task_queue_tail = t;
    } else {
        task_queue_tail->next = t;
        task_queue_tail = t;
    }
    if (t->state == TASK_READY) place_task(t, false);
}

// What a CPU runs when no task is runnable: wait for an interrupt, then see
// whether it made one runnable. Interrupts are off around the check and
// "sti; hlt" is atomic (an interrupt can not slip in between the two), so a
// wakeup is never slept through.
//
// Just before halting it sets its timer for the next thing this CPU has to
// wake for by itself (a sleeper's deadline, output to show), or switches the
// timer off if there is nothing: see timer.h.
void sched_idle_loop(void) {
    for (;;) {
        asm volatile("cli");
        // Another CPU has stopped the kernel: stop here too. (A busy CPU
        // finds out in sched_tick; an idle one has no tick to find out in.)
        if (kernel_halted) for (;;) asm volatile("cli\n\thlt");
        sched_yield();
        timer_cpu_idle();
        asm volatile("sti\n\thlt");
    }
}

static task_t *task_alloc_with_pid(const char *name, void (*entry)(void), bool assign_pid);
static void finish_switch(void);

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
    main_task->queued_on = -1;
    for (uint32_t i = 0; i < MAX_CPUS; i++) spinlock_init_ranked(&runqueues[i].lock, "runqueue", LOCK_RANK_RUNQUEUE);

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
    idle->queued_on = -1;

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

void sched_continue_on_own_stack(void (*continue_at)(void)) {
    extern void call_on_stack(uint64_t stack_top, void (*fn)(void)) __attribute__((noreturn));
    void *stack = kstack_alloc();
    if (!stack) panic("sched: no memory for PID 0's stack");
    task_t *self = sched_get_current_task();
    self->kernel_stack = stack;
    call_on_stack((uint64_t)stack + KERNEL_STACK_SIZE, continue_at);
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
    idle->queued_on = -1;
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
// brought it here was made with this CPU's queue lock held (and perhaps the
// scheduler lock), and it is this task's job to release them.
void sched_task_started(void) {
    finish_switch();
    asm volatile("sti");
}

task_t *task_alloc(const char *name, void (*entry)(void)) {
    return task_alloc_with_pid(name, entry, true);
}

// Make a task (not yet runnable) that will start in `entry`: its structure,
// a guarded kernel stack prepared so that the first switch to it lands in
// task_start_trampoline, the kernel's address space, normal priority. With
// `assign_pid`, it gets the next PID (the idle tasks get none). NULL if
// memory runs out.
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
    t->queued_on = -1;
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
    region_release_all(t);  // Likewise (but a task that never ran still holds its program's file)
    task_set_cwd(t, NULL);
    if (t->is_user && t->pml4_phys && (uint64_t)t->pml4_phys != vmm_get_kernel_pml4_phys()) {
        vmm_destroy_address_space(vmm_space_from_phys((uint64_t)t->pml4_phys));
    }
    if (t->kernel_stack) {
        kstack_free(t->kernel_stack);
    }
    kfree(t->io_buffer);
    kfree(t);
}

void task_set_affinity(task_t *task, int32_t cpu_id) {
    if (!task) return;
    uint64_t flags = sched_lock_irqsave();
    cpu_t *cpu = cpu_id >= 0 ? cpu_get((uint32_t)cpu_id) : NULL;
    // A task tied to a CPU that is not scheduling would never run again
    task->affinity = (cpu && cpu->online) ? cpu_id : TASK_ANY_CPU;

    // Waiting in the queue of a CPU it may no longer run on: move it. (The
    // queue may change under us until its lock is held: it may have been
    // taken by its CPU, or stolen, meanwhile.) A running task moves when it
    // next gives up its CPU (schedule).
    for (;;) {
        int32_t on = task->queued_on;
        if (on < 0 || task->affinity == TASK_ANY_CPU || task->affinity == on) break;
        runqueue_t *rq = &runqueues[on];
        spinlock_acquire(&rq->lock);
        bool moved = false;
        if (task->queued_on == on) {
            task_t *prev = NULL;
            for (task_t *t = rq->head; t && t != task; t = t->run_next) prev = t;
            rq_unlink(rq, prev, task);
            moved = true;
        }
        spinlock_release(&rq->lock);
        if (moved) place_task(task, true);
        if (moved) break;
    }
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

void sched_set_init(task_t *task) {
    uint64_t flags = sched_lock_irqsave();
    init_task = task;
    if (task) task->detached = true; // It has no parent to wait for it
    sched_unlock_irqrestore(flags);
}

uint32_t sched_init_pid(void) {
    uint64_t flags = sched_lock_irqsave();
    uint32_t pid = init_task ? init_task->pid : 0;
    sched_unlock_irqrestore(flags);
    return pid;
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

// A task for this CPU, whose queue is empty, from another CPU's queue: the
// oldest one not bound to its CPU, from the queue with the most waiting
// that has one. Queues are tried busiest first (ties from the CPU after
// this one, so that CPUs with equally long queues are not always passed
// over for the lowest numbered), and a queue holding only bound tasks is
// passed over: it may look busiest and have nothing to give (the test
// runner, bound to CPU 0, waits in its queue at every tick). Each other
// queue's lock is taken with a try-lock, a few times, while we hold ours:
// waiting for it outright could deadlock with that CPU taking from us.
// NULL if there was nothing to take. Our queue lock held.
static task_t *steal(uint32_t here) {
    uint32_t order[MAX_CPUS], waiting[MAX_CPUS], n = 0;
    for (uint32_t k = 1; k < MAX_CPUS; k++) {
        uint32_t i = (here + k) % MAX_CPUS;
        uint32_t count = runqueues[i].count;
        if (count == 0 || !cpu_get(i)->online) continue;
        uint32_t at = n++;                          // Busiest first; equal ones keep their order
        while (at > 0 && waiting[at - 1] < count) {
            order[at] = order[at - 1];
            waiting[at] = waiting[at - 1];
            at--;
        }
        order[at] = i;
        waiting[at] = count;
    }
    for (uint32_t v = 0; v < n; v++) {
        runqueue_t *other = &runqueues[order[v]];
        for (int tries = 0; tries < 64; tries++) {
            if (spinlock_try_acquire(&other->lock)) {
                task_t *t = rq_take(other, true);
                spinlock_release(&other->lock);
                if (t) return t;
                break;                              // Only bound tasks there: the next queue
            }
            smp_tlb_service();                      // (As any spin must: see spinlock_acquire)
            asm volatile("pause");
        }
    }
    return NULL;
}

// The first thing a task does once it is running again after a switch (or
// for the first time): release the queue lock the outgoing task held across
// the switch, and the scheduler lock if it held that too; then place a task
// that had to leave this CPU (now that it is off its stack). Interrupts
// stay off.
static void finish_switch(void) {
    runqueue_t *rq = &runqueues[this_cpu()->cpu_id];
    bool sched_too = rq->release_sched;
    task_t *moving = rq->moving;
    rq->release_sched = false;
    rq->moving = NULL;
    spinlock_release(&rq->lock);
    if (sched_too) spinlock_release(&sched_lock);
    if (moving) place_task(moving, true);
}

// Give the CPU to another task. Interrupts are off; `holding_sched` says
// whether the caller holds the scheduler lock (it has, then, set the
// current task's state if it is not to stay runnable). Takes this CPU's
// queue lock, and returns, with neither lock held and interrupts still
// off, when this task is next chosen to run (possibly on another CPU) or
// at once if it may carry on.
static void schedule(bool holding_sched) {
    cpu_t *cpu = this_cpu();
    // Rule 4 of spinlock.h: a task that switches away holding some other
    // lock would leave it locked for as long as it sleeps
    if (lock_debug_held_count() > (holding_sched ? 1u : 0u)) {
        lock_debug_report("task switch while holding", cpu->held[0]->name, NULL);
    }
    uint32_t here = cpu->cpu_id;
    runqueue_t *rq = &runqueues[here];
    spinlock_acquire(&rq->lock);
    task_t *old_task = cpu->current;
    cpu->resched_kicked = 0;

    // The next in our queue, or one from the busiest other queue. A task in
    // a queue is READY and on no CPU: whoever pushed it there either was
    // not running it, or finished switching away before releasing that
    // queue's lock.
    task_t *next_task = rq_take(rq, false);
    if (!next_task) next_task = steal(here);

    // The current task may only carry on here if it is still allowed on
    // this CPU (its affinity may have been changed while it ran)
    bool may_stay = old_task->state == TASK_RUNNING &&
                    (old_task->affinity == TASK_ANY_CPU || old_task->affinity == (int32_t)here);

    if (!next_task) {
        if (may_stay) {             // Nobody else wants the CPU: carry on
            spinlock_release(&rq->lock);
            if (holding_sched) spinlock_release(&sched_lock);
            return;
        }
        next_task = cpu->idle;      // We can not carry on: idle until someone can
    }

    if (old_task->state == TASK_RUNNING) {
        old_task->state = TASK_READY;
        if (old_task != cpu->idle) {                     // (The idle task is in no queue)
            if (may_stay) {
                rq_push(here, old_task);                 // To the back of the line
                if (rq->count > 1) kick_an_idle_cpu();   // More waiting than this CPU will run soon
            } else {
                rq->moving = old_task;                   // Elsewhere, once it is off this stack
            }
        }
    }
    if (old_task == cpu->idle) timer_cpu_busy(); // (next_task is not the idle task: it was running already)
    next_task->state = TASK_RUNNING;
    next_task->last_cpu = here;
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

    rq->release_sched = holding_sched;
    ctx_switch((uint64_t **)&old_task->rsp, (uint64_t *)next_task->rsp, next_task->pml4_phys);

    // Back in old_task, possibly much later and on a different CPU (`cpu`
    // and `rq` above are no longer ours), with the locks held by whoever
    // switched to us and interrupts off
    finish_switch();
}

void sched_yield(void) {
    if (!scheduler_active || !current_task) return;
    uint64_t flags = irq_save();
    schedule(false);
    irq_restore(flags);
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
static void wakeup_locked(void *channel);
static void stop_current(task_t *self);

// Make a blocked task runnable: forget what it waited for and when it was to
// wake, and tell an idle CPU (another may take it before this one looks).
// Caller holds the scheduler lock.
static void wake_task(task_t *t) {
    t->wait_channel = NULL;
    t->wake_tick = 0;
    t->wake_ns = 0;
    place_task(t, true);
}

// `wake_tick` and `wake_ns` are two ways of naming the moment at which the
// sleep ends whatever happens (0 for both: only a wake-up ends it).
static void sleep_on(void *channel, spinlock_t *lock, uint64_t wake_tick, uint64_t wake_ns, bool interruptible) {
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

        // Asked to stop: do it here instead of sleeping. When the process is
        // continued it returns to its caller, which finds its condition
        // unchanged and comes back to sleep: being stopped and continued is
        // not an interruption.
        if (self->stop_pending) {
            if (lock && !caller_has_sched_lock) spinlock_release(lock);
            stop_current(self);                  // (Returns holding no scheduler lock)
            if (caller_has_sched_lock) {
                spinlock_acquire(&sched_lock);
            } else {
                if (lock) spinlock_acquire(lock);
                irq_restore(flags);
            }
            return;
        }

        // A signal with a handler is waiting: do not sleep on it. The caller
        // sees sched_interrupted() and abandons its wait with EINTR.
        if (signal_deliverable(self)) {
            if (!caller_has_sched_lock) sched_unlock_irqrestore(flags);
            return;
        }
    }

    self->wait_channel = channel;
    self->wake_tick = wake_tick;
    self->wake_ns = wake_ns;
    self->state = TASK_BLOCKED;

    // With one-shot timers nothing looks at sleepers regularly: this CPU's
    // timer has to be set for this one. (This CPU, because it is the one
    // that is certain to pass through here: see sched_wake_sleepers.)
    if (timer_is_one_shot() && (wake_tick || wake_ns)) {
        timer_sleeper_deadline(wake_ns ? wake_ns : timer_tick_to_ns(wake_tick));
    }

    if (lock && !caller_has_sched_lock) spinlock_release(lock);
    schedule(true); // Returns once someone has woken us and we have been chosen to run, holding no scheduler lock

    int die = (interruptible && self->kill_pending) ? self->kill_pending : 0; // Woken to die
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
    sleep_on(channel, lock, wake_tick, 0, true);
}

void sched_sleep_uninterruptible(void *channel, spinlock_t *lock, uint64_t wake_tick) {
    sleep_on(channel, lock, wake_tick, 0, false);
}

void sched_sleep_on_task_list(void *channel) {
    sleep_on(channel, &sched_lock, 0, 0, true);
}

void sched_sleep_until_ns(uint64_t deadline_ns) {
    if (deadline_ns != 0) sleep_on(NULL, NULL, 0, deadline_ns, true);
}

// The kernel's own reaper (see sched_reap) sleeps here until there is a
// dead task that nobody else will wait for.
static int reaper_channel;

// Is there a dead task that nobody will wait for (one sched_reap frees)? Caller holds the scheduler lock.
static bool reapable_exists(void) {
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->state == TASK_ZOMBIE && t->detached && t != current_task) return true;
    }
    return false;
}

void sched_wait_for_reapable(void) {
    uint64_t flags = sched_lock_irqsave();
    // Checked under the lock that the wake-up is sent under, and only once:
    // the caller reaps and comes back
    if (!reapable_exists()) sleep_on(&reaper_channel, &sched_lock, 0, 0, false);
    sched_unlock_irqrestore(flags);
}

bool sched_interrupted(void) {
    task_t *self = current_task;
    return self && signal_deliverable(self);
}

void sched_interrupt_sleep(task_t *task) {
    if (task->state == TASK_BLOCKED) wake_task(task);
}

void sched_make_unkillable(task_t *task) {
    uint64_t flags = sched_lock_irqsave();
    task->exiting = true;
    task->kill_pending = 0;
    task->stop_pending = 0;
    task->sig_pending = 0;
    sched_unlock_irqrestore(flags);
}

void sched_request_kill(task_t *task, int sig) {
    if (task->state != TASK_ZOMBIE && !task->exiting && task->kill_pending == 0) {
        task->kill_pending = sig;
        task->stop_pending = 0;
        // A sleeper would otherwise not notice until its event happens, and
        // a stopped process never would
        if (task->state == TASK_BLOCKED || task->state == TASK_STOPPED) wake_task(task);
    }
}

void sched_request_stop(task_t *task, int sig) {
    if (task->state == TASK_ZOMBIE || task->state == TASK_STOPPED || task->exiting || task->kill_pending) return;
    task->stop_pending = sig;
    // A sleeper is woken so that it can stop (it goes back to its sleep
    // when it is continued: see sleep_on)
    if (task->state == TASK_BLOCKED) wake_task(task);
}

void sched_continue(task_t *task) {
    task->stop_pending = 0;
    if (task->state == TASK_STOPPED) wake_task(task);
}

// Suspend the current task. Caller holds the scheduler lock and nothing
// else, with interrupts off; returns when the task has been continued,
// holding neither scheduler lock (see schedule).
static void stop_current(task_t *self) {
    self->stop_signal = self->stop_pending;
    self->stop_pending = 0;
    self->stop_reported = false;
    self->state = TASK_STOPPED;

    // The parent may be waiting to hear of this (waitpid with WUNTRACED)
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->pid == self->parent_pid && t != self) {
            wakeup_locked(t);
            signal_send_locked(t, SIGCHLD);
            break;
        }
    }
    schedule(true);
}

void sched_check_kill(void) {
    task_t *self = current_task;
    if (!self) return;
    if (self->kill_pending) task_exit_status(128 + self->kill_pending);

    // The same safe point serves for giving up pages a truncation has cut
    // off from shared mappings' files (cheap when nothing was truncated)
    if (self->is_user && self->seen_truncations != pcache_truncations()) region_revalidate(self);

    // The same safe point serves for being stopped: the task holds nothing
    if (self->stop_pending) {
        uint64_t flags = sched_lock_irqsave();
        if (self->stop_pending && !self->kill_pending) {
            stop_current(self);                  // (Returns holding no scheduler lock)
            irq_restore(flags);
        } else {
            sched_unlock_irqrestore(flags);
        }
        if (self->kill_pending) task_exit_status(128 + self->kill_pending); // Killed while stopped
    }
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

// Each CPU is responsible for the deadlines of the tasks that went to sleep
// on it (a blocked task's last_cpu): it set its timer when they did, and it
// asks here what to set it to next. Any CPU that looks wakes every task
// that is due, whoever's it is.
uint64_t sched_wake_sleepers(uint64_t now_ticks, uint64_t now_ns) {
    uint64_t earliest = 0;
    uint64_t flags = sched_lock_irqsave();
    uint32_t here = this_cpu()->cpu_id;
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->state != TASK_BLOCKED) continue;
        if (now_ns == 0) {
            // Periodic tick: only deadlines given in ticks exist
            if (t->wake_tick != 0 && now_ticks >= t->wake_tick) wake_task(t);
            continue;
        }
        uint64_t at = t->wake_ns ? t->wake_ns : t->wake_tick ? timer_tick_to_ns(t->wake_tick) : 0;
        if (at == 0) continue;
        if (now_ns >= at) wake_task(t);
        else if (t->last_cpu == here && (earliest == 0 || at < earliest)) earliest = at;
    }
    sched_unlock_irqrestore(flags);
    return earliest;
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

    // No scheduler lock: the counts and the turn belong to this CPU's task,
    // which only this CPU touches (and interrupts are off)
    uint64_t flags = irq_save();
    task_t *self = current_task;
    self->cpu_ticks++;
    // How busy this CPU is: an idle CPU is not ticked with one-shot timers,
    // and its idle task's ticks are not counted with the periodic one, so
    // the count means the same in both modes
    if (self != this_cpu()->idle) this_cpu()->busy_ticks++;

    // Preempt only when the running task has used up its turn. The idle
    // task has no turn: it gives way as soon as anything is runnable.
    if (self != this_cpu()->idle && self->state == TASK_RUNNING && self->slice_left > 1) {
        self->slice_left--;
    } else {
        schedule(false);
    }
    irq_restore(flags);
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
    self->in_syscall = 0;   // Dying, not in a call (exit, or one it was killed in, never returns): before its files close
    sched_unlock_irqrestore(flags);

    // Close its files now rather than when it is reaped: a reader at the
    // other end of a pipe must see end-of-file as soon as the writer dies.
    // (Done before taking the scheduler lock: closing takes other locks
    // and may sleep.)
    file_close_all(self->fds);
    task_set_cwd(self, NULL);

    // Its memory goes now too, not when someone collects its exit status: a
    // parent may take its time over that (or be asleep), and until then a
    // large dead program would hold frames, and swap slots, that others are
    // being refused. From here on the task runs on the kernel's own page
    // tables (it is in the kernel, on its kernel stack, and will not return
    // to user code). The task is pointed at them before the CPU is, with
    // interrupts off, so that being switched out and in again in between
    // loads the right ones; and under the scheduler lock, which /proc holds
    // while it walks a task's page tables (vmm_count_user_pages): a walk
    // under way finishes before the pointer changes, and none starts on the
    // tables once they are being freed.
    if (self->is_user && self->pml4_phys && (uint64_t)self->pml4_phys != vmm_get_kernel_pml4_phys()) {
        uint64_t *space = vmm_space_from_phys((uint64_t)self->pml4_phys);
        uint64_t irq = sched_lock_irqsave();
        self->pml4_phys = (uint64_t *)vmm_get_kernel_pml4_phys();
        vmm_switch_pml4(self->pml4_phys);
        sched_unlock_irqrestore(irq);
        vmm_destroy_address_space(space);
    }
    // Its regions go once its pages have (what shared mappings wrote is
    // written to their files, and a page nobody maps any more is left
    // clean), and its mapped files are free to be removed again
    region_release_all(self);
    tty_process_exited(self->pid);
    process_foreground_owner_exited(self->pid);

    sched_lock_irqsave(); // Never released by this task: see sched_lock
    self->exit_status = status;
    self->state = TASK_ZOMBIE;
    if (self->detached) wakeup_locked(&reaper_channel);

    // Children of a dead task have nobody left to wait for them: the init
    // process takes them over, and will wait for them in its turn. (If there
    // is none, they are marked for the kernel's own reaper.)
    if (init_task == self) init_task = NULL;
    task_t *parent = NULL;
    bool adopted = false;
    for (task_t *t = task_queue_head; t; t = t->next) {
        if (t->parent_pid == self->pid && t != self) {
            if (init_task) {
                t->parent_pid = init_task->pid;
                adopted = true;
            } else {
                t->detached = true;
                if (t->state == TASK_ZOMBIE) wakeup_locked(&reaper_channel); // Dead already, and now nobody's
            }
        }
        if (t->pid == self->parent_pid && t != self) parent = t;
    }
    if (adopted) {
        wakeup_locked(init_task);
        signal_send_locked(init_task, SIGCHLD);
    }

    // A parent blocked in process_wait sleeps on its own task struct; one
    // that would rather be told gets SIGCHLD
    if (parent) {
        wakeup_locked(parent);
        signal_send_locked(parent, SIGCHLD);
    }

    // A zombie is never chosen again. Whoever sees its state from now on
    // may free it, stack included: the scheduler lock is held until this
    // switch is complete.
    schedule(true);
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
            case TASK_STOPPED: state_str = "STOPPED"; break;
        }
        kprintf("%d\t%s\t%s\n", curr->pid, state_str, curr->name);
        curr = curr->next;
    }
    sched_unlock_irqrestore(flags);
}
