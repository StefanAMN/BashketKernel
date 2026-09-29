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

/**
 * @brief Put the current task to sleep until sched_wakeup(channel) is called
 *        or, if wake_tick is non-zero, until that timer tick.
 *
 * Call with interrupts disabled, in a loop that re-checks the condition being
 * waited for (see the comment in sched.c). Returns with interrupts disabled.
 */
void sched_block(void *channel, uint64_t wake_tick);

/**
 * @brief Like sched_block, but a kill or a signal does not cut the sleep
 *        short and never ends the task here. For waits during which the task
 *        owns something that must be released in an orderly way, such as a
 *        disk request in progress. Such waits must be short and certain to
 *        end. The task may still be woken early; callers loop on their
 *        condition as usual.
 */
void sched_block_uninterruptible(void *channel, uint64_t wake_tick);

/**
 * @brief True if the current task has a signal waiting for its handler. Code
 *        that waits in a loop around sched_block must check this after each
 *        wakeup and give up (EINTR) instead of going back to sleep:
 *
 *     while (!condition) {
 *         if (sched_interrupted()) { ...undo...; return -EINTR; }
 *         sched_block(channel, 0);
 *     }
 *
 * sched_block itself returns at once while this is true.
 */
bool sched_interrupted(void);

/**
 * @brief Wake a task out of whatever it is blocked on, without the event it
 *        was waiting for having happened.
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
 */
void sched_request_kill(task_t *task, int sig);

/**
 * @brief Safe point: end the current task now if a kill was requested.
 */
void sched_check_kill(void);

/**
 * @brief Mark a task dead: record its status, orphan its children and wake
 *        its parent. The task must not be running on another CPU.
 */
void sched_make_zombie(task_t *task, int status);

/**
 * @brief Change a task's working directory (NULL: the root).
 */
void task_set_cwd(task_t *task, struct vnode *dir);

/**
 * @brief Remove one zombie from the task list and free it. The caller must
 *        not be the zombie. On one CPU a zombie other than the caller is never
 *        still on its kernel stack, so freeing it at once is safe.
 */
void sched_free_zombie(task_t *task);

/**
 * @brief Head of the run queue, for walking all tasks via ->next.
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
