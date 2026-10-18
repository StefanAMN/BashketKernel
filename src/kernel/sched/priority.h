/**
 * @file priority.h
 * @brief Task priorities
 *
 * The scheduler is round robin with weighted time slices: each runnable task
 * runs in turn, and its priority decides for how many timer ticks before it
 * is preempted. A high-priority task therefore gets a larger share of the
 * CPU, but a low-priority one is never starved.
 */

#ifndef PRIORITY_H
#define PRIORITY_H

#include <stdint.h>
#include "task.h"

/**
 * @brief Set a task's priority. Values above PRIO_LOW are treated as PRIO_LOW.
 *        Takes effect from its next turn.
 */
void task_set_priority(task_t *task, uint8_t priority);

/**
 * @brief A task's priority (PRIO_NORMAL for NULL).
 */
uint8_t task_get_priority(const task_t *task);

/**
 * @brief Time slice, in timer ticks, for a priority.
 */
uint8_t priority_time_slice(uint8_t priority);

#endif // PRIORITY_H
