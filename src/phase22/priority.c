/**
 * @file priority.c
 * @brief Implementation of Task Priority Level Management
 */

#include "priority.h"

static uint8_t task_priorities[256] = {1}; // Default PRIO_NORMAL

void task_set_priority(task_t *task, uint8_t priority) {
    if (!task || task->pid >= 256) return;
    if (priority > PRIO_LOW) priority = PRIO_LOW;
    task_priorities[task->pid] = priority;
}

uint8_t task_get_priority(const task_t *task) {
    if (!task || task->pid >= 256) return PRIO_NORMAL;
    return task_priorities[task->pid];
}
