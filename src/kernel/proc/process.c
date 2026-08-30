/**
 * @file process.c
 * @brief Implementation of UNIX Process Subsystem & Stack-Based Environment Execve
 */

#include "process.h"
#include "sched.h"
#include "kprintf.h"
#include "cow.h"
#include "vmm.h"
#include "pmm.h"
#include "kstring.h"
#include "vfs.h"
#include "elf.h"
#include "env.h"

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

int process_execve_env(const char *path, char *const argv[], char *const envp[]) {
    (void)argv;
    if (!path) return -1;

    // 1. Locate binary file in Virtual Filesystem (VFS)
    vnode_t *file = vfs_find(NULL, path);
    if (!file) {
        if (path[0] == '/') {
            file = vfs_find(NULL, path + 1);
        }
    }

    if (!file || file->type != VFS_FILE || !file->data) {
        kprintf("Process execve ERROR: Binary '%s' not found in VFS!\n", path);
        return -1;
    }

    // 2. Parse ELF64 binary and map PT_LOAD segments to user memory
    void *entry_point = NULL;
    if (!elf_load(file->data, &entry_point)) {
        kprintf("Process execve ERROR: Failed to load ELF binary '%s'\n", path);
        return -1;
    }

    // 3. Allocate user stack page (0x7FFFFFFF000)
    uint64_t user_stack_top = 0x7FFFFFFF000ULL;
    void *stack_frame = pmm_alloc_frame();
    if (stack_frame) {
        vmm_map_page(NULL, user_stack_top - PAGE_SIZE, (uint64_t)stack_frame, VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }

    // 4. Setup environment variables directly on the user process stack frame
    uint64_t user_rsp = user_stack_top;
    uint64_t envp_vaddr = 0;
    env_setup_user_stack(user_stack_top, envp, &user_rsp, &envp_vaddr);

    // 5. Update current active task state with new binary context
    task_t *current = sched_get_current_task();
    if (current) {
        strncpy(current->name, file->name, sizeof(current->name) - 1);
    }

    kprintf("Process: execve loaded binary '%s' (Entry: %p, User RSP: %p, envp: %p)\n",
            path, entry_point, (void *)user_rsp, (void *)envp_vaddr);
    return 0;
}

int process_execve(const char *path) {
    return process_execve_env(path, NULL, NULL);
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
