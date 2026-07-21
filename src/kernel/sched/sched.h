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
