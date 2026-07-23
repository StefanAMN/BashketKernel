/**
 * @file priority.c
 * @brief Task priorities (see priority.h for the scheduling policy)
 */

#include "priority.h"

// Timer ticks per turn, indexed by priority. At 100 Hz: 40, 20 and 10 ms.
static const uint8_t time_slices[] = {
    [PRIO_HIGH]   = 4,
    [PRIO_NORMAL] = 2,
    [PRIO_LOW]    = 1,
};

void task_set_priority(task_t *task, uint8_t priority) {
    if (!task) return;
    if (priority > PRIO_LOW) priority = PRIO_LOW;
    task->priority = priority;
}

uint8_t task_get_priority(const task_t *task) {
    return task ? task->priority : PRIO_NORMAL;
}

uint8_t priority_time_slice(uint8_t priority) {
    if (priority > PRIO_LOW) priority = PRIO_LOW;
    return time_slices[priority];
}
