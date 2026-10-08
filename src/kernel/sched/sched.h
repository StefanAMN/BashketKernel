/**
 * @file sched.h
 * @brief Preemptive Multitasking Round-Robin Scheduler Interface
 */

#ifndef SCHED_H
#define SCHED_H

#include "task.h"

/**
 * @brief Initialize Multitasking Scheduler and register main kernel thread (PID 0).
 */
void sched_init(void);

/**
 * @brief Create a new kernel thread.
 * @param name Descriptive task name.
 * @param entry Function pointer to task entry point.
 * @return Pointer to newly created task control block.
 */
task_t *task_create(const char *name, void (*entry)(void));

/**
 * @brief Allocate a task and its kernel stack without making it runnable.
 * The caller may fill in more fields, then must call sched_add().
 */
task_t *task_alloc(const char *name, void (*entry)(void));

/**
 * @brief Put a task from task_alloc() on the run queue.
 */
void sched_add(task_t *t);

/**
 * @brief Free a task that is not on the run queue: its address space (if it
 * owns one), its kernel stack and the task struct.
 */
void task_free(task_t *t);

/**
 * @brief Look up a task on the run queue by PID (NULL if there is none).
 */
task_t *sched_find_task(uint32_t pid);

// The same, for a caller that holds the scheduler lock (and so may go on to
// use the task before anyone can free it)
task_t *sched_find_task_locked(uint32_t pid);

/**
 * @brief Give CPU `cpu_id` an idle task. Called on the boot CPU before that
 *        CPU is started: the new CPU must not allocate while starting.
 */
task_t *sched_prepare_cpu(uint32_t cpu_id);

/**
 * @brief Called on a newly started CPU: from here on it takes part in
 *        scheduling. Its caller then goes into sched_idle_loop().
 */
void sched_join_this_cpu(void);

/**
 * @brief What a CPU does when it has no task to run. Never returns.
 */
void sched_idle_loop(void);

/**
 * @brief Tie a task to one CPU, or (TASK_ANY_CPU) let it run on any. Takes
 *        effect the next time the task is scheduled. A CPU that is not
 *        scheduling is treated as TASK_ANY_CPU.
 */
void task_set_affinity(task_t *task, int32_t cpu_id);

/**
 * @brief How many tasks on the task list are running at this instant (one
 *        per CPU at most; idle CPUs do not count).
 */
uint32_t sched_running_count(void);

/**
 * @brief Put the current task to sleep until sched_wakeup(channel) is called
 *        or, if wake_tick is non-zero, until that timer tick.
 *
 * `lock` protects the condition being waited for. The caller holds it (taken
 * with interrupts off); it is released while the task sleeps and held again
 * on return. Because waking can be spurious, always call this in a loop that
 * re-checks the condition.
 *
 * `lock` may be NULL only when a missed wakeup does no harm: a pure timed
 * sleep, or a wait that polls with a deadline.
 *
 * A kill ends the task here (after releasing `lock`). A signal with a
 * handler makes it return at once without sleeping: see sched_interrupted().
 */
struct spinlock;
void sched_sleep(void *channel, struct spinlock *lock, uint64_t wake_tick);

/**
 * @brief Like sched_sleep, but a kill or a signal does not cut the sleep
 *        short. For waits inside the kernel that hold something (a device
 *        request in flight, a volume lock) and must finish first.
 */
void sched_sleep_uninterruptible(void *channel, struct spinlock *lock, uint64_t wake_tick);

/**
 * @brief sched_sleep for a condition on the task list itself: the caller
 *        holds the scheduler lock (sched_lock_irqsave) and holds it again
 *        on return.
 */
void sched_sleep_on_task_list(void *channel);

/**
 * @brief Take and release the scheduler lock, for code outside the
 *        scheduler that walks the task list (sched_first_task) or changes
 *        the fields it protects (see sched.c).
 */
uint64_t sched_lock_irqsave(void);
void sched_unlock_irqrestore(uint64_t flags);

/**
 * @brief Whether a signal is waiting for the current task's handler. Code
 *        that waits in a loop around sched_sleep must check this after each
 *        return and give up with -EINTR if it is true:
 *
 *     while (!condition) {
 *         if (sched_interrupted()) { ...undo...; return -EINTR; }
 *         sched_sleep(channel, &lock, 0);
 *     }
 *
 * sched_sleep itself returns at once while this is true.
 */
bool sched_interrupted(void);

/**
 * @brief Wake a task out of whatever it is blocked on, without the event it
 *        was waiting for having happened. Caller holds the scheduler lock.
 */
void sched_interrupt_sleep(task_t *task);

/**
 * @brief Wake every task blocked on `channel`. Safe to call from interrupts.
 */
void sched_wakeup(void *channel);

/**
 * @brief Wake tasks whose wake_tick has passed. Called from the timer interrupt.
 */
void sched_wake_sleepers(uint64_t now);

/**
 * @brief Sleep for at least `ticks` timer ticks without using CPU time.
 * @return true if the full time passed, false if a signal cut the sleep short.
 */
bool sched_sleep_ticks(uint64_t ticks);

/**
 * @brief Ask for a process to be ended. It does not die here: it may be in
 *        the middle of a kernel operation that must finish. It dies at its
 *        next safe point: when it would sleep, when a system call is about to
 *        return, or when an interrupt is about to return to its user code.
 *        Caller holds the scheduler lock.
 */
void sched_request_kill(task_t *task, int sig);

/**
 * @brief Safe point: end the current task now if a kill was requested.
 */
void sched_check_kill(void);


/**
 * @brief Change a task's working directory (NULL: the root).
 */
void task_set_cwd(task_t *task, struct vnode *dir);

/**
 * @brief Take a zombie off the task list. The caller holds the scheduler
 *        lock, and calls task_free() on it after releasing the lock.
 */
void sched_unlink_zombie(task_t *task);

/**
 * @brief Head of the task list, for walking all tasks via ->next. Hold the
 *        scheduler lock while walking.
 */
task_t *sched_first_task(void);

/**
 * @brief Terminate the current task with an exit status. Does not return.
 */
void task_exit_status(int status);

/**
 * @brief Voluntarily yield CPU to the next ready task in round-robin queue.
 */
void sched_yield(void);

/**
 * @brief Preemptive timer tick handler called by PIT IRQ 0.
 */
void sched_tick(void);

/**
 * @brief Get currently executing task control block.
 * @return Pointer to active task.
 */
task_t *sched_get_current_task(void);

/**
 * @brief Terminate current task execution.
 */
void task_exit(void);

/**
 * @brief Free the task structs and kernel stacks of all exited tasks.
 * @return Number of tasks reaped.
 */
size_t sched_reap(void);

/**
 * @brief Display process list table for ps shell command.
 */
void sched_print_tasks(void);

#endif // SCHED_H
