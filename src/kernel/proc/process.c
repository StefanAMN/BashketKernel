/**
 * @file process.c
 * @brief User process creation and lifecycle
 */

#include "process.h"
#include "sched.h"
#include "kprintf.h"
#include "vmm.h"
#include "pmm.h"
#include "mmap.h"
#include "kstring.h"
#include "vfs.h"
#include "elf.h"
#include "gdt.h"
#include "spinlock.h"
#include "file.h"

extern void enter_user_mode(uint64_t rip, uint64_t rsp);
extern void user_frame_return(void);

// Everything needed to start a program in a fresh address space
typedef struct {
    uint64_t *pml4;
    uint64_t entry;
    uint64_t rsp;
    uint64_t image_end;
    char name[32];
} user_image_t;

static uint64_t page_align_up(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

// Lay out argc/argv on the new program's stack the way the System V AMD64
// ABI specifies for process entry:
//
//   high addresses   argument strings
//                    padding to 16 bytes
//                    NULL                  (end of envp)
//                    NULL                  (end of argv)
//                    argv[argc-1] ... argv[0]
//   RSP ->           argc
//
// RSP must be 16-byte aligned at entry. The stack belongs to another address
// space, so it is written through vmm_copy_to_space.
static bool build_user_stack(uint64_t *pml4, const char *path, char *const argv[], uint64_t *out_rsp) {
    uint64_t stack_bottom = USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    if (user_map_range(pml4, stack_bottom, USER_STACK_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }

    char *const default_argv[] = { (char *)path, NULL };
    if (!argv || !argv[0]) argv = default_argv;

    uint64_t arg_addrs[PROCESS_MAX_ARGS];
    size_t argc = 0;
    size_t string_bytes = 0;
    uint64_t sp = USER_STACK_TOP;

    for (; argv[argc] != NULL; argc++) {
        if (argc >= PROCESS_MAX_ARGS) return false;

        size_t len = strlen(argv[argc]) + 1;
        string_bytes += len;
        if (string_bytes > PROCESS_ARG_BYTES) return false;

        sp -= len;
        if (vmm_copy_to_space(pml4, sp, argv[argc], len) != len) return false;
        arg_addrs[argc] = sp;
    }

    sp &= ~0xFULL;

    // argc + argv pointers + two NULLs; pad so the final RSP is 16-byte aligned
    size_t slots = 1 + argc + 2;
    if (slots % 2) sp -= 8;

    uint64_t vec[PROCESS_MAX_ARGS + 3];
    vec[0] = argc;
    for (size_t i = 0; i < argc; i++) vec[1 + i] = arg_addrs[i];
    vec[1 + argc] = 0;
    vec[2 + argc] = 0;

    sp -= slots * sizeof(uint64_t);
    if (vmm_copy_to_space(pml4, sp, vec, slots * sizeof(uint64_t)) != slots * sizeof(uint64_t)) return false;

    *out_rsp = sp;
    return true;
}

// Build a complete user image for `path`. On failure nothing is left allocated.
static bool load_user_image(const char *path, char *const argv[], user_image_t *image) {
    vnode_t *file = vfs_find(NULL, path);
    if (!file || file->type != VFS_FILE || !file->data) return false;

    image->pml4 = vmm_create_address_space();
    if (!image->pml4) return false;

    if (!elf_load(image->pml4, file->data, file->size, &image->entry, &image->image_end) ||
        !build_user_stack(image->pml4, path, argv, &image->rsp)) {
        vmm_destroy_address_space(image->pml4);
        image->pml4 = NULL;
        return false;
    }

    strncpy(image->name, file->name, sizeof(image->name) - 1);
    image->name[sizeof(image->name) - 1] = '\0';
    return true;
}

static void apply_image(task_t *task, const user_image_t *image) {
    task->is_user = true;
    task->pml4_phys = (uint64_t *)((uint64_t)image->pml4 - pmm_get_hhdm_offset());
    task->user_entry = image->entry;
    task->user_rsp = image->rsp;
    task->brk_start = page_align_up(image->image_end);
    task->brk_end = task->brk_start;
    task->mmap_next = USER_MMAP_BASE;
    strncpy(task->name, image->name, sizeof(task->name) - 1);
    task->name[sizeof(task->name) - 1] = '\0';
}

// First code a new process runs, in ring 0 on its own kernel stack. The
// scheduler has already loaded its address space and pointed TSS.RSP0 at this
// stack, so all that is left is the drop to ring 3.
static void user_task_start(void) {
    task_t *self = sched_get_current_task();
    enter_user_mode(self->user_entry, self->user_rsp);
}

pid_t process_spawn(const char *path, char *const argv[]) {
    if (!path) return -1;

    user_image_t image;
    if (!load_user_image(path, argv, &image)) return -1;

    task_t *task = task_alloc(image.name, user_task_start);
    if (!task) {
        vmm_destroy_address_space(image.pml4);
        return -1;
    }

    apply_image(task, &image);
    task->detached = false; // The creator is expected to process_wait() for it

    // Standard input, output and error all start as the console
    file_t *console = file_open_console();
    if (!console) {
        task_free(task);
        return -1;
    }
    task->fds[0] = console;
    task->fds[1] = file_dup(console);
    task->fds[2] = file_dup(console);

    pid_t pid = (pid_t)task->pid;
    sched_add(task);
    return pid;
}

int process_exec(struct registers *regs, const char *path, char *const argv[]) {
    task_t *self = sched_get_current_task();
    if (!self || !self->is_user || !regs || !path) return -1;

    // Build the new image completely before touching the old one, so a
    // failed exec returns to a program that still exists.
    user_image_t image;
    if (!load_user_image(path, argv, &image)) return -1;

    uint64_t *old_pml4 = vmm_space_from_phys((uint64_t)self->pml4_phys);
    apply_image(self, &image);

    // Leave the old address space before freeing it
    vmm_switch_pml4(self->pml4_phys);
    vmm_destroy_address_space(old_pml4);

    // Rewrite the syscall frame: the return to ring 3 starts the new program
    memset(regs, 0, sizeof(*regs));
    regs->rip = image.entry;
    regs->rsp = image.rsp;
    regs->cs = USER_CS;
    regs->ss = USER_DS;
    regs->rflags = 0x202;
    return 0;
}

pid_t process_fork(struct registers *regs) {
    task_t *parent = sched_get_current_task();
    if (!parent || !parent->is_user || !regs) return -1;

    uint64_t *child_pml4 = vmm_create_address_space();
    if (!child_pml4) return -1;

    // Share every page copy-on-write: nothing is copied until one side writes
    if (!vmm_clone_user_space(child_pml4, vmm_space_from_phys((uint64_t)parent->pml4_phys))) {
        vmm_destroy_address_space(child_pml4);
        return -1;
    }

    task_t *child = task_alloc(parent->name, NULL);
    if (!child) {
        vmm_destroy_address_space(child_pml4);
        return -1;
    }

    child->is_user = true;
    child->detached = false;
    child->pml4_phys = (uint64_t *)((uint64_t)child_pml4 - pmm_get_hhdm_offset());
    child->brk_start = parent->brk_start;
    child->brk_end = parent->brk_end;
    child->mmap_next = parent->mmap_next;

    // The child shares the parent's open files, positions included
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        child->fds[fd] = file_dup(parent->fds[fd]);
    }

    // The child's kernel stack starts with a copy of the parent's syscall
    // frame, except that its syscall "returns" 0. Below the frame goes the
    // context that ctx_switch pops, with user_frame_return as the return
    // address, so the first switch to the child lands in ring 3 right after
    // the parent's syscall instruction.
    uint64_t stack_top = ((uint64_t)child->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
    struct registers *child_regs = (struct registers *)(stack_top - sizeof(struct registers));
    *child_regs = *regs;
    child_regs->rax = 0;

    uint64_t *sp = (uint64_t *)child_regs;
    *(--sp) = (uint64_t)user_frame_return; // Return address for ctx_switch's ret
    *(--sp) = 0x002;                       // RFLAGS: interrupts off until iretq
    *(--sp) = 0;                           // R15
    *(--sp) = 0;                           // R14
    *(--sp) = 0;                           // R13
    *(--sp) = 0;                           // R12
    *(--sp) = 0;                           // RBP
    *(--sp) = 0;                           // RBX
    child->rsp = (uint64_t)sp;

    pid_t pid = (pid_t)child->pid;
    sched_add(child);
    return pid;
}

pid_t process_wait(pid_t pid, int *status) {
    task_t *self = sched_get_current_task();
    if (!self) return -1;

    // Interrupts stay off between looking for a dead child and going to sleep,
    // so a child that exits in between cannot be missed (see sched_block).
    uint64_t flags = irq_save();
    pid_t found = -1;

    while (1) {
        bool have_child = false;

        for (task_t *t = sched_first_task(); t; t = t->next) {
            if (t->parent_pid != self->pid || t == self || t->detached) continue;
            if (pid != -1 && (pid_t)t->pid != pid) continue;

            have_child = true;
            if (t->state == TASK_ZOMBIE) {
                if (status) *status = t->exit_status;
                found = (pid_t)t->pid;
                t->detached = true; // Collected: the idle loop may free it now
                break;
            }
        }

        if (found != -1 || !have_child) break;

        // sched_make_zombie wakes this channel when a child of ours dies
        sched_block(self, 0);
    }

    irq_restore(flags);
    return found;
}

int process_kill(pid_t pid, int sig) {
    task_t *target = sched_find_task((uint32_t)pid);
    if (!target || !target->is_user || target->state == TASK_ZOMBIE) return -1;

    if (target == sched_get_current_task()) {
        task_exit_status(128 + sig);
    }

    // The target is not running (one CPU, and we are), so it holds no lock
    // and can simply never be scheduled again. Its parent collects it.
    sched_make_zombie(target, 128 + sig);
    return 0;
}
