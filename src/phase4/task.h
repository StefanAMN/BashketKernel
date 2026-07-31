/**
 * @file task.h
 * @brief Task Control Block (TCB) and Process State Definitions
 * 
 * Defines task structures for kernel threads and userspace processes.
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define KERNEL_STACK_SIZE (16 * 1024) // 16 KiB kernel stack per task

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

typedef struct task {
    uint64_t rsp;            // Saved kernel stack pointer (points to saved registers frame)
    uint32_t pid;            // Process ID
    char name[32];           // Task description name
    task_state_t state;      // Current execution state
    uint64_t *pml4_phys;     // Physical address of PML4 (for virtual address space)
    void *kernel_stack;      // Virtual base pointer of 16 KiB kernel stack
    struct task *next;       // Next task link in scheduler queue
} task_t;

#endif // TASK_H
