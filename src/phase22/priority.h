/**
 * @file priority.h
 * @brief Priority-Based Multilevel Scheduler Interface
 */

#ifndef PRIORITY_H
#define PRIORITY_H

#include <stdint.h>
#include "task.h"

#define PRIO_HIGH   0
#define PRIO_NORMAL 1
#define PRIO_LOW    2

/**
 * @brief Set execution priority level for a task.
 * @param task Target task control block.
 * @param priority Priority level (PRIO_HIGH, PRIO_NORMAL, PRIO_LOW).
 */
void task_set_priority(task_t *task, uint8_t priority);

/**
 * @brief Get execution priority level of a task.
 * @param task Target task control block.
 * @return Priority level value.
 */
uint8_t task_get_priority(const task_t *task);

#endif // PRIORITY_H
