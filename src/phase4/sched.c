/**
 * @file sched.c
 * @brief Implementation of Round-Robin Preemptive Multitasking Scheduler
 */

#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "kprintf.h"
#include "string_util.h"

extern void ctx_switch(uint64_t **old_rsp_ptr, uint64_t *new_rsp, uint64_t *new_pml4_phys);

static task_t *current_task = NULL;
static task_t *task_queue_head = NULL;
static task_t *task_queue_tail = NULL;
static uint32_t next_pid = 0;
static bool scheduler_active = false;

static void enqueue_task(task_t *t) {
    t->next = NULL;
    if (task_queue_head == NULL) {
        task_queue_head = t;
        task_queue_tail = t;
    } else {
        task_queue_tail->next = t;
        task_queue_tail = t;
    }
}

void sched_init(void) {
    // Create PID 0 representation for the main running kernel execution context
    task_t *main_task = (task_t *)kmalloc(sizeof(task_t));
    memset(main_task, 0, sizeof(task_t));
    main_task->pid = next_pid++;
    main_task->name[0] = 'k'; main_task->name[1] = 'e'; main_task->name[2] = 'r'; main_task->name[3] = 'n';
    main_task->name[4] = 'e'; main_task->name[5] = 'l'; main_task->name[6] = '_'; main_task->name[7] = 'm';
    main_task->name[8] = 'a'; main_task->name[9] = 'i'; main_task->name[10] = 'n'; main_task->name[11] = '\0';
    main_task->state = TASK_RUNNING;
    main_task->pml4_phys = NULL; // Uses kernel PML4
    main_task->next = NULL;

    current_task = main_task;
    enqueue_task(main_task);

    scheduler_active = true;
    kprintf("Scheduler Init: Preemptive multitasking enabled (Main Kernel PID: %d)\n", main_task->pid);
}

task_t *task_create(const char *name, void (*entry)(void)) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(task_t));

    t->pid = next_pid++;
    if (name) {
        size_t i = 0;
        for (; name[i] != '\0' && i < sizeof(t->name) - 1; i++) {
            t->name[i] = name[i];
        }
        t->name[i] = '\0';
    } else {
        t->name[0] = 't'; t->name[1] = 'a'; t->name[2] = 's'; t->name[3] = 'k'; t->name[4] = '\0';
    }

    // Allocate 16 KiB (4 frames) kernel stack
    void *stack_phys = pmm_alloc_frames(4);
    if (!stack_phys) {
        kfree(t);
        return NULL;
    }

    uint64_t hhdm = pmm_get_hhdm_offset();
    t->kernel_stack = (void *)((uint64_t)stack_phys + hhdm);
    t->state = TASK_READY;
    t->pml4_phys = NULL; // Shares kernel PML4 address space

    // Build initial stack frame for ctx_switch
    uint64_t *stack_top = (uint64_t *)((uint8_t *)t->kernel_stack + KERNEL_STACK_SIZE);

    *(--stack_top) = (uint64_t)task_exit;  // Return address if entry returns
    *(--stack_top) = (uint64_t)entry;      // Target entry point (popped by ret in ctx_switch)
    *(--stack_top) = 0x202;                // RFLAGS (Interrupts enabled 0x200 | bit 1 set)
    *(--stack_top) = 0;                    // R15
    *(--stack_top) = 0;                    // R14
    *(--stack_top) = 0;                    // R13
    *(--stack_top) = 0;                    // R12
    *(--stack_top) = 0;                    // RBP
    *(--stack_top) = 0;                    // RBX

    t->rsp = (uint64_t)stack_top;

    enqueue_task(t);
    kprintf("Task Created: PID %d ('%s') at Entry: %p\n", t->pid, t->name, entry);
    return t;
}

task_t *sched_get_current_task(void) {
    return current_task;
}

void sched_yield(void) {
    if (!scheduler_active || !current_task) return;

    task_t *old_task = current_task;
    task_t *next_task = NULL;

    // Find next TASK_READY task in queue
    task_t *curr = old_task->next;
    while (curr != old_task) {
        if (!curr) {
            curr = task_queue_head;
        }
        if (curr->state == TASK_READY || curr->state == TASK_RUNNING) {
            next_task = curr;
            break;
        }
        curr = curr->next;
    }

    if (!next_task || next_task == old_task) {
        return; // No other task ready to run
    }

    if (old_task->state == TASK_RUNNING) {
        old_task->state = TASK_READY;
    }
    next_task->state = TASK_RUNNING;
    current_task = next_task;

    ctx_switch((uint64_t **)&old_task->rsp, (uint64_t *)next_task->rsp, next_task->pml4_phys);
}

void sched_tick(void) {
    if (scheduler_active) {
        sched_yield();
    }
}

void task_exit(void) {
    if (!current_task) return;

    kprintf("Task Exit: PID %d ('%s') finished execution.\n", current_task->pid, current_task->name);
    current_task->state = TASK_ZOMBIE;
    sched_yield();

    // Should never reach here
    while (1) { asm volatile("hlt"); }
}
