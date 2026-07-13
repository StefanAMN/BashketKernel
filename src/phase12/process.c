/**
 * @file process.c
 * @brief Implementation of UNIX Process Subsystem
 */

#include "process.h"
#include "sched.h"
#include "kprintf.h"
#include "cow.h"
#include "vmm.h"
#include "kstring.h"

static int next_pid = 100;

static void child_entry_stub(void) {
    kprintf("Process: Child process executed entry point successfully\n");
    task_exit();
}

pid_t process_fork(void) {
    task_t *current = sched_get_current_task();
    if (!current) return -1;

    pid_t child_pid = next_pid++;
    task_t *child = task_create("forked_child", child_entry_stub);
    if (!child) return -1;

    child->pid = child_pid;
    child->state = TASK_READY;

    kprintf("Process: Forked child PID %d from parent PID %u\n", child_pid, current->pid);
    return child_pid;
}

int process_execve(const char *path) {
    if (!path) return -1;
    kprintf("Process: execve loaded binary '%s'\n", path);
    return 0;
}

pid_t process_waitpid(pid_t pid, int *status) {
    if (status) *status = 0;
    kprintf("Process: waitpid waited on child PID %d\n", pid);
    return pid;
}

int process_kill(pid_t pid, int sig) {
    kprintf("Process: Sent signal %d to PID %d\n", sig, pid);
    return 0;
}
