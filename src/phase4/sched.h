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

#endif // SCHED_H
