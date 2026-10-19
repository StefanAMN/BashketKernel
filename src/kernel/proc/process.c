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
#include "fpu.h"
#include "random.h"
#include "auth.h"
#include "heap.h"
#include "swap.h"

extern void enter_user_mode(uint64_t rip, uint64_t rsp);
extern void user_frame_return(void);
extern void task_start_trampoline(void);

// Everything needed to start a program in a fresh address space
typedef struct {
    uint64_t *pml4;
    uint64_t entry;
    uint64_t rsp;
    uint64_t image_end;
    uint64_t stack_top;    // Randomised per program
    char name[32];
    bool setuid;          // The file has the set-uid bit...
    uint32_t owner;       // ...and this owner
} user_image_t;

// Lay out the arguments and environment on the new program's stack the way
// the System V AMD64 ABI specifies for process entry:
//
//   high addresses   argument and environment strings
//                    padding to 16 bytes
//                    NULL                  (end of envp)
//                    envp[envc-1] ... envp[0]
//                    NULL                  (end of argv)
//                    argv[argc-1] ... argv[0]
//   RSP ->           argc
//
// RSP must be 16-byte aligned at entry. The stack belongs to another address
// space, so it is written through vmm_copy_to_space.

// Copy one NULL-terminated vector of strings below *sp and record where each
// string went. Returns the number of strings, or -1 if there are too many or
// they are too long in total.
static int push_strings(uint64_t *pml4, char *const strings[], uint64_t *sp, size_t *bytes_used, uint64_t *addrs) {
    int count = 0;
    for (; strings && strings[count] != NULL; count++) {
        if (count >= PROCESS_MAX_ARGS) return -1;

        size_t len = strlen(strings[count]) + 1;
        *bytes_used += len;
        if (*bytes_used > PROCESS_ARG_BYTES) return -1;

        *sp -= len;
        if (vmm_copy_to_space(pml4, *sp, strings[count], len) != len) return -1;
        addrs[count] = *sp;
    }
    return count;
}

// Map a new program's stack (USER_STACK_PAGES below `stack_top`) and lay
// out on it what the program's start code expects: the strings of argv and
// envp, then argc, the argv pointers, NULL, the envp pointers, NULL (the
// System V layout). argv defaults to { path }. *out_rsp receives the stack
// pointer the program starts with. False if memory runs out or the
// arguments do not fit.
static bool build_user_stack(uint64_t *pml4, uint64_t stack_top, const char *path, char *const argv[], char *const envp[], uint64_t *out_rsp) {
    uint64_t stack_bottom = stack_top - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    if (user_map_range(pml4, stack_bottom, USER_STACK_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }

    char *const default_argv[] = { (char *)path, NULL };
    if (!argv || !argv[0]) argv = default_argv;

    uint64_t arg_addrs[PROCESS_MAX_ARGS], env_addrs[PROCESS_MAX_ARGS];
    size_t string_bytes = 0;
    uint64_t sp = stack_top;

    int argc = push_strings(pml4, argv, &sp, &string_bytes, arg_addrs);
    int envc = push_strings(pml4, envp, &sp, &string_bytes, env_addrs);
    if (argc < 0 || envc < 0) return false;

    sp &= ~0xFULL;

    // argc, the argv pointers and their NULL, the envp pointers and their
    // NULL; pad so that the final RSP is 16-byte aligned
    size_t slots = 1 + (size_t)argc + 1 + (size_t)envc + 1;
    if (slots % 2) sp -= 8;

    uint64_t vec[2 * PROCESS_MAX_ARGS + 3];
    size_t n = 0;
    vec[n++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) vec[n++] = arg_addrs[i];
    vec[n++] = 0;
    for (int i = 0; i < envc; i++) vec[n++] = env_addrs[i];
    vec[n++] = 0;

    sp -= slots * sizeof(uint64_t);
    if (vmm_copy_to_space(pml4, sp, vec, slots * sizeof(uint64_t)) != slots * sizeof(uint64_t)) return false;

    *out_rsp = sp;
    return true;
}

static bool load_user_image_locked(const char *path, char *const argv[], char *const envp[], user_image_t *image);

// Load the program at `path` into a new address space, with its arguments
// and environment on the stack, ready for a task to start in (`image`). The
// caller must be allowed to execute and read it. False if not, or if it is
// not a valid program, or memory runs out; nothing is left allocated then.
static bool load_user_image(const char *path, char *const argv[], char *const envp[], user_image_t *image) {
    // The file is used from the moment its name is looked up until its
    // contents have been copied into the new address space: it must not be
    // removed or rewritten meanwhile
    vfs_lock();
    bool ok = load_user_image_locked(path, argv, envp, image);
    vfs_unlock();
    return ok;
}

// Build a complete user image for `path`. On failure nothing is left allocated.
// load_user_image, with the file tree locked by the caller
static bool load_user_image_locked(const char *path, char *const argv[], char *const envp[], user_image_t *image) {
    task_t *caller = sched_get_current_task();
    vnode_t *file = vfs_find(caller ? caller->cwd : NULL, path);
    if (!file || file->type != VFS_FILE || file->size == 0) return false;
    if (!auth_may_execute(auth_current_uid(), file)) return false;       // Marked as a program, for this user?
    if (!auth_may_access(auth_current_uid(), file, false)) return false; // Running a program means reading it
    image->setuid = (file->mode & PERM_SETUID) != 0;
    image->owner = file->uid;

    // A file in memory (the initrd) can be loaded from where it is. One whose
    // contents come from elsewhere (the disk) is read into a buffer first.
    const void *contents = file->data;
    void *loaded = NULL;
    if (file->ops) {
        if (file->size > PROCESS_MAX_IMAGE_BYTES) return false;
        loaded = kmalloc(file->size);
        if (!loaded) return false;
        if (vfs_read(file, 0, file->size, loaded) != file->size) {
            kfree(loaded);
            return false;
        }
        contents = loaded;
    }
    if (!contents) return false;

    // Address-space layout randomisation: the stack starts up to 16 MiB
    // below its highest possible position (the program itself is placed by
    // the loader, the mmap region by apply_image)
    image->stack_top = USER_STACK_TOP - ((uint64_t)(get_random_u32() & 0xFFF) << 12);

    image->pml4 = vmm_create_address_space();
    bool ok = image->pml4 != NULL &&
              elf_load(image->pml4, contents, file->size, &image->entry, &image->image_end) &&
              build_user_stack(image->pml4, image->stack_top, path, argv, envp, &image->rsp);
    kfree(loaded);

    if (!ok) {
        if (image->pml4) vmm_destroy_address_space(image->pml4);
        image->pml4 = NULL;
        return false;
    }

    strncpy(image->name, file->name, sizeof(image->name) - 1);
    image->name[sizeof(image->name) - 1] = '\0';
    return true;
}

// Make `task` run the loaded program: its address space, entry point and
// stack, an empty heap after the image, a randomly placed start for mmap,
// and (set-uid) the file owner's rights
static void apply_image(task_t *task, const user_image_t *image) {
    task->is_user = true;
    task->pml4_phys = (uint64_t *)((uint64_t)image->pml4 - pmm_get_hhdm_offset());
    task->user_entry = image->entry;
    task->user_rsp = image->rsp;
    task->brk_start = page_align_up(image->image_end);
    task->brk_end = task->brk_start;
    task->mmap_next = USER_MMAP_BASE + ((uint64_t)(get_random_u32() & 0xFFFFF) << 12); // Up to 4 GiB in
    task->stack_top = image->stack_top;
    fpu_state_init(task->fpu_state); // A new program starts with clean FPU registers
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

// The stack starts small and grows when the program touches the page below
// what it has. The page fault handler calls this for "page not present"
// faults. The lowest page of the region is never mapped: running into it is
// how unbounded recursion ends (with a fault) instead of reaching other memory.
bool process_populate(uint64_t addr, size_t len) {
    task_t *self = sched_get_current_task();
    if (!self || !self->is_user || len == 0) return true;
    if (addr >= USER_SPACE_END || len > USER_SPACE_END - addr) return false;

    uint64_t *space = vmm_space_from_phys((uint64_t)self->pml4_phys);
    for (uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1); page < addr + len; page += PAGE_SIZE) {
        uint64_t entry = vmm_get_pte(space, page);
        if (entry & VMM_PRESENT) continue;
        if (vmm_pte_swapped(entry)) {
            // On the swap disk: read it back. (Nothing is pushed out to
            // make room here: see swap.h.)
            if (swap_in(space, page) < 0) return false;
            continue;
        }
        if (process_grow_stack(page)) continue;
        int given = region_fault(self, page);
        if (given < 0) return false;  // Promised, and no memory to keep the promise
        // Never promised: the range is bad, and the check that follows will
        // say so. Stop here: the length is the caller's word, and walking
        // terabytes of nothing one page at a time is not a way to say no.
        if (given == 0) break;
    }
    return true;
}

bool process_grow_stack(uint64_t addr) {
    task_t *self = sched_get_current_task();
    if (!self || !self->is_user) return false;
    if (self->stack_top < USER_STACK_MAX) return false;
    uint64_t limit = self->stack_top - USER_STACK_MAX;
    if (addr < limit + PAGE_SIZE || addr >= self->stack_top) return false;

    uint64_t *space = vmm_space_from_phys((uint64_t)self->pml4_phys);
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t entry = vmm_get_pte(space, page);
    if ((entry & VMM_PRESENT) || vmm_pte_swapped(entry)) return false; // Some other kind of fault

    return vmm_alloc_user_page(space, page, VMM_WRITABLE | VMM_NO_EXECUTE) != NULL;
}

pid_t process_spawn(const char *path, char *const argv[]) {
    if (!path) return -1;

    // A process started by the kernel has no parent to inherit from, so it
    // gets a minimal environment
    static char *const default_env[] = { "PATH=/bin", "HOME=/", NULL };

    user_image_t image;
    if (!load_user_image(path, argv, default_env, &image)) return -1;

    task_t *task = task_alloc(image.name, user_task_start);
    if (!task) {
        vmm_destroy_address_space(image.pml4);
        return -1;
    }

    apply_image(task, &image);
    task->detached = false; // The creator is expected to process_wait() for it
    task->pgid = task->pid; // A process group of its own
    task->umask = UMASK_DEFAULT;

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

int process_exec(struct registers *regs, const char *path, char *const argv[], char *const envp[]) {
    task_t *self = sched_get_current_task();
    if (!self || !self->is_user || !regs || !path) return -1;

    // Build the new image completely before touching the old one, so a
    // failed exec returns to a program that still exists.
    user_image_t image;
    if (!load_user_image(path, argv, envp, &image)) return -1;

    uint64_t *old_pml4 = vmm_space_from_phys((uint64_t)self->pml4_phys);
    region_release_all(self); // The old program's mappings are not the new one's
    apply_image(self, &image);

    // A set-uid program runs as the file's owner, whoever started it. The
    // real user id stays: it is how the program knows who that was.
    if (image.setuid) self->uid = image.owner;

    signal_reset_on_exec(self);

    // Descriptors marked close-on-exec belong to the old program only
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        if (!(self->fd_cloexec & (1u << fd))) continue;
        file_t *file = self->fds[fd];
        self->fds[fd] = NULL;
        file_close(file);
    }
    self->fd_cloexec = 0;

    // The old program's floating point registers are still live in the CPU
    fpu_restore(self->fpu_state);

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

    // Sharing instead of copying postpones the cost of fork: every page
    // either side writes to afterwards needs a frame then, and a process
    // that cannot get one is killed in the middle of whatever it was doing.
    // So refuse now, while the caller can still be told, rather than
    // succeed with nothing left over for those copies.
    if (pmm_get_free_frames() < PROCESS_FORK_RESERVE_FRAMES) {
        task_free(child);
        vmm_destroy_address_space(child_pml4);
        return -1;
    }

    child->is_user = true;
    child->detached = false;
    child->pml4_phys = (uint64_t *)((uint64_t)child_pml4 - pmm_get_hhdm_offset());
    child->brk_start = parent->brk_start;
    child->brk_end = parent->brk_end;
    child->uid = parent->uid;
    child->ruid = parent->ruid;
    child->gid = parent->gid;
    child->umask = parent->umask;
    child->mmap_next = parent->mmap_next;
    region_fork(child, parent);
    child->stack_top = parent->stack_top;
    memcpy(child->sig_handlers, parent->sig_handlers, sizeof(child->sig_handlers));
    memcpy(child->sig_masks, parent->sig_masks, sizeof(child->sig_masks));
    memcpy(child->sig_flags, parent->sig_flags, sizeof(child->sig_flags));
    child->sig_blocked = parent->sig_blocked; // What is pending for the parent is the parent's alone
    child->sig_restorer = parent->sig_restorer;
    task_set_cwd(child, parent->cwd);

    // The parent is running, so its FPU registers are in the CPU, not in its
    // task struct: save them straight into the child.
    fpu_save(child->fpu_state);

    // The child shares the parent's open files, positions included
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        child->fds[fd] = file_dup(parent->fds[fd]);
    }
    child->fd_cloexec = parent->fd_cloexec;
    child->pgid = parent->pgid;

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
    *(--sp) = (uint64_t)user_frame_return; // Where the trampoline returns to
    *(--sp) = (uint64_t)task_start_trampoline; // Return address for ctx_switch's ret: releases the scheduler lock
    *(--sp) = 0x002;                       // RFLAGS: interrupts off
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
    return process_wait_options(pid, status, 0);
}

pid_t process_wait_options(pid_t pid, int *status, int options) {
    task_t *self = sched_get_current_task();
    if (!self) return -1;

    // The scheduler lock is held from looking for a dead child until we are
    // asleep, so a child that exits in between cannot be missed.
    uint64_t flags = sched_lock_irqsave();
    pid_t found = -1;
    task_t *collected = NULL;

    while (1) {
        bool have_child = false;

        for (task_t *t = sched_first_task(); t; t = t->next) {
            if (t->parent_pid != self->pid || t == self || t->detached) continue;
            if (pid != -1 && (pid_t)t->pid != pid) continue;

            have_child = true;
            if (t->state == TASK_STOPPED && !t->stop_reported && (options & PROCESS_WUNTRACED)) {
                // Stopped, not dead: reported once, and the child stays
                t->stop_reported = true;
                if (status) *status = PROCESS_STATUS_STOPPED | t->stop_signal;
                found = (pid_t)t->pid;
                break;
            }
            if (t->state == TASK_ZOMBIE) {
                if (status) *status = t->exit_status;
                found = (pid_t)t->pid;
                sched_unlink_zombie(t);
                collected = t;
                break;
            }
        }

        if (found != -1 || !have_child) break;

        if (options & PROCESS_WNOHANG) {
            found = 0; // There are children, none of them dead: say so instead of waiting
            break;
        }

        if (sched_interrupted()) {
            found = PROCESS_WAIT_INTERRUPTED;
            break;
        }

        // A dying task wakes this channel if we are its parent
        sched_sleep_on_task_list(self);
    }

    sched_unlock_irqrestore(flags);

    // Collected: its memory goes back right now (not under the lock: this
    // frees an address space)
    if (collected) task_free(collected);
    return found;
}

int process_kill(pid_t pid, int sig) {
    if (sig <= 0 || sig >= TASK_NSIG) return -1;

    // Found and signalled under one hold of the lock, so the task can not
    // be reaped in between
    int result = -1;
    uint64_t flags = sched_lock_irqsave();
    task_t *target = sched_find_task_locked((uint32_t)pid);
    if (target && target->is_user) {
        // The target may be half way through a system call, so nothing is
        // done to it here: the signal is recorded and takes effect at its
        // next safe point. That is true even when the target is the caller.
        // (Sending to a process that is already dead but not yet waited
        // for succeeds and does nothing.)
        if (target->state != TASK_ZOMBIE) signal_send_locked(target, sig);
        result = 0;
    }
    sched_unlock_irqrestore(flags);
    return result;
}

// The process group that has the terminal: signals typed at the keyboard go
// to it. 0: nobody has claimed it, and Ctrl-C goes to every user process
// (which is what the kernel shell, which knows no groups, relies on). Read
// from interrupt handlers, so plain words rather than something locked.
static volatile uint32_t foreground_pgid = 0;
static volatile uint32_t foreground_owner = 0; // The shell that set it; cleared when that exits

int process_set_foreground(uint32_t pgid) {
    task_t *self = sched_get_current_task();
    if (!self) return -1;
    foreground_owner = pgid ? self->pid : 0;
    foreground_pgid = pgid;
    return 0;
}

uint32_t process_get_foreground(void) {
    return foreground_pgid;
}

void process_foreground_owner_exited(uint32_t pid) {
    // A shell that dies must not leave the keyboard aimed at one of its jobs
    if (foreground_owner == pid) {
        foreground_pgid = 0;
        foreground_owner = 0;
    }
}

bool process_signal_foreground(int sig) {
    uint32_t pgid = foreground_pgid;
    if (pgid == 0 && sig != SIGINT) return false; // Only Ctrl-C has a meaning without job control

    uint64_t flags = sched_lock_irqsave();
    for (task_t *t = sched_first_task(); t; t = t->next) {
        if (t->is_user && (pgid == 0 || t->pgid == pgid)) signal_send_locked(t, sig);
    }
    sched_unlock_irqrestore(flags);
    return true;
}

void process_interrupt_all(void) {
    process_signal_foreground(SIGINT);
}

int process_kill_group(uint32_t pgid, int sig, uint32_t sender_uid) {
    if (pgid == 0 || sig <= 0 || sig >= TASK_NSIG) return -1;

    int sent = 0;
    uint64_t flags = sched_lock_irqsave();
    for (task_t *t = sched_first_task(); t; t = t->next) {
        if (!t->is_user || t->pgid != pgid || t->state == TASK_ZOMBIE) continue;
        if (!auth_may_signal(sender_uid, t->uid)) continue;
        signal_send_locked(t, sig);
        sent++;
    }
    sched_unlock_irqrestore(flags);
    return sent > 0 ? 0 : -1;
}

int process_set_group(pid_t pid, uint32_t pgid) {
    task_t *self = sched_get_current_task();
    if (!self) return -1;
    if (pid == 0) pid = (pid_t)self->pid;
    if (pgid == 0) pgid = (uint32_t)pid;

    int result = -1;
    uint64_t flags = sched_lock_irqsave();
    task_t *target = sched_find_task_locked((uint32_t)pid);
    // Only for oneself or one's own child, and only into a group of one's
    // own making: the target's own, or one that a relative already leads
    if (target && target->is_user && (target == self || target->parent_pid == self->pid)) {
        bool exists = pgid == target->pid;
        for (task_t *t = sched_first_task(); t && !exists; t = t->next) {
            if (t->pgid == pgid && (t == self || t->parent_pid == self->pid || t->parent_pid == self->parent_pid)) exists = true;
        }
        if (exists) {
            target->pgid = pgid;
            result = 0;
        }
    }
    sched_unlock_irqrestore(flags);
    return result;
}

pid_t process_spawn_init(const char *path, char *const argv[]) {
    pid_t pid = process_spawn(path, argv);
    if (pid < 0) return pid;
    sched_set_init(sched_find_task((uint32_t)pid));
    return pid;
}
