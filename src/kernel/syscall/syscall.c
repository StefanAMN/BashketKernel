/**
 * @file syscall.c
 * @brief System call dispatcher and MSR configuration
 *
 * Every pointer argument comes from a user program and is untrusted. Nothing
 * here dereferences one before user_range_ok() has confirmed it lies in the
 * caller's own mapped memory with the right permission, and every access is
 * made between user_access_begin() and user_access_end() (see cpu.h): on a
 * CPU with SMAP, touching user memory anywhere else is a fault.
 */

#include "syscall.h"
#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include "sched.h"
#include "pit.h"
#include "vfs.h"
#include "keyboard.h"
#include "mmap.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "kstring.h"
#include "gdt.h"
#include "heap.h"
#include "file.h"
#include "priority.h"
#include "cpu.h"
#include "tty.h"
#include "net.h"
#include "tcp.h"
#include "shm.h"
#include "hrtimer.h"
#include "walltime.h"
#include "poll.h"
#include "power.h"
#include "auth.h"

extern void syscall_entry(void);

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_AC (1ULL << 18)

#define PATH_MAX VFS_PATH_MAX

void syscall_enable_on_this_cpu(void) {
    // 1. Enable SCE (System Call Extensions), bit 0 of EFER
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1ULL);

    // 2. STAR selects the segments:
    //    bits 47..32: SYSCALL loads CS from this value and SS from value + 8
    //                 -> 0x08 kernel code, 0x10 kernel data
    //    bits 63..48: SYSRET loads SS from value + 8 and CS from value + 16
    //                 -> 0x10 gives SS 0x18 (user data), CS 0x20 (user code)
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)GDT_KERNEL_DATA << 48);
    wrmsr(MSR_STAR, star);

    // 3. LSTAR: where SYSCALL jumps
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // 4. SFMASK: RFLAGS bits cleared on entry. IF, because the entry stub runs
    //    on the user's stack until it switches; DF, because compiled C code
    //    assumes the direction flag is clear.
    //    AC, so a system call starts with the user access window closed even
    //    if the program set the flag itself (see cpu.h).
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_AC);
}

void syscall_init(void) {
    syscall_enable_on_this_cpu();

    kprintf("Syscall Init: MSRs configured for syscall/sysret (LSTAR: %p)\n", syscall_entry);
}

// --- User memory access -----------------------------------------------------

static uint64_t *current_space(void) {
    task_t *task = sched_get_current_task();
    return vmm_space_from_phys((uint64_t)task->pml4_phys);
}

// May the calling process's memory [addr, addr + len) be read (and, with
// `write`, written) by the kernel on its behalf? Pages it was promised but
// has not touched get their frames first, and pages on the swap disk are
// read back (process_populate). Use the range only inside
// user_access_begin/end.
static bool user_range_ok(uint64_t addr, size_t len, bool write) {
    // Memory the program has been promised but has not touched yet is
    // given its frames now: see process_populate
    if (!process_populate(addr, len)) return false;
    return vmm_user_range_ok(current_space(), addr, len, write);
}

// Copy a NUL-terminated string from user memory into a kernel buffer.
// Returns its length; -EFAULT if it is unreadable; -ENAMETOOLONG if it does
// not fit (with its NUL) in dst_size bytes.
static int64_t copy_string_from_user(char *dst, uint64_t user_src, size_t dst_size) {
    for (size_t i = 0; i < dst_size; i++) {
        // Check once per page, not once per byte
        if (i == 0 || ((user_src + i) & (PAGE_SIZE - 1)) == 0) {
            if (!user_range_ok(user_src + i, 1, false)) return -EFAULT;
        }
        user_access_begin();
        dst[i] = *(const char *)(user_src + i);
        user_access_end();
        if (dst[i] == '\0') return (int64_t)i;
    }
    return -ENAMETOOLONG; // No terminator within dst_size
}

// --- Individual system calls -------------------------------------------------

// Look up a descriptor in the calling process's table
static file_t *fd_to_file(int fd) {
    task_t *task = sched_get_current_task();
    if (fd < 0 || fd >= TASK_MAX_FDS) return NULL;
    return task->fds[fd];
}

// read() and write() do not hand the user's buffer to the file layer. The
// data goes through a page of kernel memory, and the user access window
// (see cpu.h) is open only for the copy between the two. Everything below
// the system call (pipes, the terminal, filesystems, drivers) therefore
// only ever sees kernel pointers and runs with the window closed, however
// long it sleeps: a stray use of a user pointer down there faults instead of
// working.
//
// The page belongs to the task and lives as long as it does, so a process
// killed in the middle of a read (it may be asleep in one for hours) leaks
// nothing.
#define IO_BUFFER_SIZE PAGE_SIZE

// The calling task's page for read() and write() to copy through, made on
// first use and freed with the task. NULL if memory runs out.
static uint8_t *io_buffer(task_t *task) {
    if (!task->io_buffer) task->io_buffer = kmalloc(IO_BUFFER_SIZE);
    return task->io_buffer;
}

// read(fd, buf, count): read up to `count` bytes into buf. A file is read
// until the request is met or the file ends; a terminal, pipe or socket
// gives what it has (waiting for something first). Returns the number of
// bytes, 0 at the end; -EBADF, -EFAULT, or the file's error if nothing was read.
static int64_t sys_read(int fd, uint64_t buf, size_t count) {
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    if (count == 0) return 0;
    if (!user_range_ok(buf, count, true)) return -EFAULT;

    uint8_t *kernel_buf = io_buffer(sched_get_current_task());
    if (!kernel_buf) return -ENOMEM;

    size_t done = 0;
    while (done < count) {
        size_t want = count - done < IO_BUFFER_SIZE ? count - done : IO_BUFFER_SIZE;
        int64_t n = file_read(file, kernel_buf, want); // May sleep (console, pipe)
        if (n < 0) return done > 0 ? (int64_t)done : n;

        user_access_begin();
        memcpy((void *)(buf + done), kernel_buf, (size_t)n);
        user_access_end();
        done += (size_t)n;

        // A terminal or a pipe gives what it has and the caller comes back
        // for more: asking again here could wait for ever with data in
        // hand. A file is read until the request is met or it ends.
        if ((size_t)n < want || file->kind != FILE_VNODE) break;
    }
    return (int64_t)done;
}

// write(fd, buf, count): write `count` bytes from buf. Returns how many were
// written (fewer if the file stopped taking them); -EBADF, -EFAULT, or the
// file's error if nothing was written.
static int64_t sys_write(int fd, uint64_t buf, size_t count) {
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    if (count == 0) return 0;
    if (!user_range_ok(buf, count, false)) return -EFAULT;

    uint8_t *kernel_buf = io_buffer(sched_get_current_task());
    if (!kernel_buf) return -ENOMEM;

    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done < IO_BUFFER_SIZE ? count - done : IO_BUFFER_SIZE;
        user_access_begin();
        memcpy(kernel_buf, (const void *)(buf + done), chunk);
        user_access_end();

        int64_t n = file_write(file, kernel_buf, chunk);
        if (n < 0) return done > 0 ? (int64_t)done : n; // Report what did go through
        done += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    return (int64_t)done;
}

// open(path, flags): open a file as the caller (permissions checked with its
// user id; O_CREAT makes the file with its umask) and give it the lowest
// free descriptor. Returns the descriptor or a negative error.
static int64_t sys_open(uint64_t user_path, int flags) {
    char path[PATH_MAX];
    int64_t path_len = copy_string_from_user(path, user_path, sizeof(path));
    if (path_len < 0) return path_len;

    file_t *file = NULL;
    task_t *task = sched_get_current_task();
    int err = file_open_as(task->uid, task->cwd, path, flags, &file);
    if (err != 0) return err;

    int fd = fd_install(task->fds, file);
    if (fd < 0) file_close(file);
    else if (flags & O_CLOEXEC) task->fd_cloexec |= 1u << fd;
    return fd;
}

// close(fd): free the descriptor; the file itself closes when its last
// descriptor (in any process) is gone. -EBADF if fd is not open.
static int64_t sys_close(int fd) {
    task_t *task = sched_get_current_task();
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    task->fds[fd] = NULL;
    task->fd_cloexec &= ~(1u << fd);
    file_close(file);
    return 0;
}

// pipe2(fds, flags): make a pipe and store its two descriptors (read end,
// then write end) in fds[0] and fds[1]. Only O_CLOEXEC may be given.
static int64_t sys_pipe(uint64_t user_fds, uint64_t flags) {
    if (flags & ~(uint64_t)O_CLOEXEC) return -EINVAL;
    if (!user_range_ok(user_fds, 2 * sizeof(int), true)) return -EFAULT;

    file_t *reader = NULL, *writer = NULL;
    int err = file_open_pipe(&reader, &writer);
    if (err != 0) return err;

    task_t *task = sched_get_current_task();
    int read_fd = fd_install(task->fds, reader);
    int write_fd = (read_fd >= 0) ? fd_install(task->fds, writer) : -EMFILE;
    if (write_fd < 0) {
        if (read_fd >= 0) task->fds[read_fd] = NULL;
        file_close(reader);
        file_close(writer);
        return -EMFILE;
    }
    if (flags & O_CLOEXEC) task->fd_cloexec |= (1u << read_fd) | (1u << write_fd);

    int *out = (int *)user_fds;
    user_access_begin();
    out[0] = read_fd;
    out[1] = write_fd;
    user_access_end();
    return 0;
}

// dup2(old_fd, new_fd): make new_fd refer to the same open file as old_fd,
// closing what new_fd was first. Returns new_fd.
static int64_t sys_dup2(int old_fd, int new_fd) {
    file_t *file = fd_to_file(old_fd);
    if (!file || new_fd < 0 || new_fd >= TASK_MAX_FDS) return -EBADF;
    if (old_fd == new_fd) return new_fd;

    task_t *task = sched_get_current_task();
    file_t *replaced = task->fds[new_fd];
    task->fds[new_fd] = file_dup(file);
    task->fd_cloexec &= ~(1u << new_fd); // The flag belongs to the descriptor, not the file: a copy starts without it
    file_close(replaced);
    return new_fd;
}

// dup(fd): a second descriptor (the lowest free) for the same open file
static int64_t sys_dup(int old_fd) {
    file_t *file = fd_to_file(old_fd);
    if (!file) return -EBADF;

    int fd = fd_install(sched_get_current_task()->fds, file);
    if (fd < 0) return fd;
    file_dup(file); // Now two descriptors refer to it
    return fd;
}

#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1

// The one thing fcntl is for here: reading and setting close-on-exec
static int64_t sys_fcntl(int fd, uint64_t command, uint64_t arg) {
    task_t *task = sched_get_current_task();
    if (!fd_to_file(fd)) return -EBADF;
    if (command == F_GETFD) return (task->fd_cloexec >> fd) & 1;
    if (command == F_SETFD) {
        if (arg & FD_CLOEXEC) task->fd_cloexec |= 1u << fd;
        else task->fd_cloexec &= ~(1u << fd);
        return 0;
    }
    return -EINVAL;
}

// readdir(fd, entry): the next entry of an open directory into *entry.
// Returns 1 for an entry, 0 at the end, or a negative error.
static int64_t sys_readdir(int fd, uint64_t user_entry) {
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    if (!user_range_ok(user_entry, sizeof(struct dirent_info), true)) return -EFAULT;

    user_access_begin();
    int result = file_readdir(file, (struct dirent_info *)user_entry);
    user_access_end();
    return result;
}

// mkdir(path): make a directory, owned by the caller, with 0777 less its
// umask. The caller needs the right to write in the parent directory.
static int64_t sys_mkdir(uint64_t user_path) {
    char path[PATH_MAX];
    int64_t path_len = copy_string_from_user(path, user_path, sizeof(path));
    if (path_len < 0) return path_len;

    char name[VFS_NAME_MAX];
    vnode_t *parent = NULL;
    int found = vfs_find_parent_at(sched_get_current_task()->cwd, path, &parent, name);
    if (found < 0) return found;

    task_t *task = sched_get_current_task();
    if (!auth_may_access(task->uid, parent, true)) return -EACCES;

    vnode_t *dir = NULL;
    int err = vfs_create(parent, name, VFS_DIRECTORY, &dir);
    if (err == 0 && (!parent->ops || !parent->ops->create || (dir->ops && dir->ops->setattr))) {
        dir->uid = task->uid;
        dir->gid = task->gid;
        dir->mode = (uint16_t)(PERM_NEW_DIR & ~task->umask);
        if (dir->ops && dir->ops->setattr) dir->ops->setattr(dir);
    }
    return err;
}

// unlink(path): remove a file or an empty directory. Except for root, the
// caller needs the right to write in the directory and must own the entry
// or the directory (so that everyone may use /tmp, but not empty it).
static int64_t sys_unlink(uint64_t user_path) {
    char path[PATH_MAX];
    int64_t path_len = copy_string_from_user(path, user_path, sizeof(path));
    if (path_len < 0) return path_len;

    char leaf[VFS_NAME_MAX];
    vnode_t *parent = NULL;
    int found = vfs_find_parent_at(sched_get_current_task()->cwd, path, &parent, leaf);
    if (found < 0) return found;

    vnode_t *child = vfs_find_child(parent, leaf);
    if (child && !auth_may_remove(sched_get_current_task()->uid, parent, child)) return -EACCES;
    return vfs_unlink(parent, leaf);
}

// rename(old, new): give an entry a new name, in the same or another
// directory of the same filesystem. Nothing is replaced: -EEXIST if `new`
// exists.
static int64_t sys_rename(uint64_t user_old, uint64_t user_new) {
    char old_path[PATH_MAX], new_path[PATH_MAX];
    int64_t old_path_len = copy_string_from_user(old_path, user_old, sizeof(old_path));
    if (old_path_len < 0) return old_path_len;
    int64_t new_path_len = copy_string_from_user(new_path, user_new, sizeof(new_path));
    if (new_path_len < 0) return new_path_len;

    task_t *task = sched_get_current_task();
    char old_name[VFS_NAME_MAX], new_name[VFS_NAME_MAX];
    vnode_t *old_parent = NULL, *new_parent = NULL;
    int found = vfs_find_parent_at(task->cwd, old_path, &old_parent, old_name);
    if (found == 0) found = vfs_find_parent_at(task->cwd, new_path, &new_parent, new_name);
    if (found < 0) return found;

    // Taking the entry out of one directory needs the right to remove it;
    // putting it into the other needs the right to write there
    vnode_t *node = vfs_find_child(old_parent, old_name);
    if (!node) return -ENOENT;
    if (!auth_may_remove(task->uid, old_parent, node)) return -EACCES;
    if (!auth_may_access(task->uid, new_parent, true)) return -EACCES;
    if (node->open_count > 0 && node->type == VFS_DIRECTORY) return -EBUSY; // Someone's working directory

    return vfs_rename(old_parent, old_name, new_parent, new_name);
}

// Find the node a path names, for the calls that change its owner or mode
static int64_t lookup_for_change(uint64_t user_path, vnode_t **out) {
    char path[PATH_MAX];
    int64_t path_len = copy_string_from_user(path, user_path, sizeof(path));
    if (path_len < 0) return path_len;
    *out = vfs_find(sched_get_current_task()->cwd, path);
    return *out ? 0 : -ENOENT;
}

// chmod(path, mode): set the permission bits. Only the owner or root.
static int64_t sys_chmod(uint64_t user_path, uint64_t mode) {
    vnode_t *node = NULL;
    int64_t err = lookup_for_change(user_path, &node);
    if (err != 0) return err;

    // Only the owner (or root) decides who may use a file
    uint32_t uid = sched_get_current_task()->uid;
    if (uid != UID_ROOT && uid != node->uid) return -EPERM;
    if (node->read_only) return -EROFS;
    if (mode & ~(uint64_t)PERM_ALL) return -EINVAL;
    node->mode = (uint16_t)mode;
    return (node->ops && node->ops->setattr) ? node->ops->setattr(node) : 0; // Where it is kept, if anywhere
}

#define CHOWN_KEEP 0xFFFFFFFFULL // "Leave this one as it is"

// chown(path, uid, gid): give a file to another user or group
// (CHOWN_KEEP leaves one as it is). Root only, except that an owner may
// move a file into its own group. Takes away the set-uid bit.
static int64_t sys_chown(uint64_t user_path, uint64_t new_uid, uint64_t new_gid) {
    vnode_t *node = NULL;
    int64_t err = lookup_for_change(user_path, &node);
    if (err != 0) return err;
    if (new_uid > 0xFFFFFFFFULL || new_gid > 0xFFFFFFFFULL) return -EINVAL;

    // Giving files away (or taking them) is root's privilege. An owner may
    // move a file of theirs into their own group.
    task_t *task = sched_get_current_task();
    bool owner_to_own_group = task->uid == node->uid && new_uid == CHOWN_KEEP && new_gid == task->gid;
    if (task->uid != UID_ROOT && !owner_to_own_group) return -EPERM;
    if (node->read_only) return -EROFS;

    if (new_uid != CHOWN_KEEP) node->uid = (uint32_t)new_uid;
    if (new_gid != CHOWN_KEEP) node->gid = (uint32_t)new_gid;
    // Whoever the file belongs to now did not ask for it to be set-uid
    if (task->uid != UID_ROOT || new_uid != CHOWN_KEEP) node->mode &= (uint16_t)~PERM_SETUID;
    return (node->ops && node->ops->setattr) ? node->ops->setattr(node) : 0;
}

// kill(pid, sig): send a signal to a process, or (pid < 0) to every process
// of group -pid. Allowed for root, or towards one's own processes.
static int64_t sys_kill(int pid, int sig) {
    // A negative number names a process group: every member gets the signal
    if (pid < 0) {
        if (sig <= 0 || sig >= TASK_NSIG) return -ESRCH;
        return process_kill_group((uint32_t)(-(int64_t)pid), sig, sched_get_current_task()->uid) == 0 ? 0 : -ESRCH;
    }

    // Look at the target under the scheduler lock: it may be reaped at any
    // other time. (PIDs are never reused, so the kill below can not reach a
    // different process.)
    uint64_t flags = sched_lock_irqsave();
    task_t *target = sched_find_task_locked((uint32_t)pid);
    // A process that has died but has not been waited for still exists, as
    // far as signals go: on another CPU it may die between two kills
    bool exists = target && target->is_user;
    uint32_t target_uid = exists ? target->uid : 0;
    sched_unlock_irqrestore(flags);
    if (!exists) return -ESRCH;
    if (!auth_may_signal(sched_get_current_task()->uid, target_uid)) return -EPERM;
    return process_kill((pid_t)pid, sig) == 0 ? 0 : -ESRCH;
}

// brk(addr): move the end of the heap to addr (0: just say where it is).
// Returns the end after the call, which is the old one if refused.
static int64_t sys_brk(uint64_t new_brk) {
    task_t *task = sched_get_current_task();
    if (new_brk == 0) return (int64_t)task->brk_end;
    return (int64_t)region_set_brk(task, new_brk);
}

// mmap(0, length, prot, flags, fd, offset): map memory at an address the
// kernel chooses: anonymous (private: promised and given frames when
// touched; shared: given frames at once and shared with forked children),
// or a private copy of part of a file opened for reading. Returns the
// address or a negative error.
static int64_t sys_mmap(uint64_t addr, size_t length, int prot, int flags, int fd, uint64_t offset) {
    // Always at an address the kernel chooses
    if (addr != 0 || (flags & MAP_FIXED) || length == 0) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;

    task_t *task = sched_get_current_task();
    bool shared = (flags & MAP_SHARED) != 0;
    if (shared == ((flags & MAP_PRIVATE) != 0)) return -EINVAL; // One or the other

    if (flags & MAP_ANONYMOUS) {
        if (shared) {
            // Shared with children: given its frames at once, so that parent
            // and child are certain to get the same ones
            uint64_t mapped = user_mmap_anon_flags(current_space(), &task->mmap_next, length, prot, true);
            return mapped ? (int64_t)mapped : -ENOMEM;
        }
        int error = 0;
        uint64_t mapped = region_map(task, length, prot, NULL, 0, &error);
        return mapped ? (int64_t)mapped : error;
    }

    // A file: private only (changes stay in this process), and the file must
    // have been opened for reading, which is what its pages will be
    if (shared) return -EINVAL;
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    if (file->kind != FILE_VNODE || file->node->type != VFS_FILE || !file->readable) return -EACCES;
    if (offset & (PAGE_SIZE - 1)) return -EINVAL;

    int error = 0;
    uint64_t mapped = region_map(task, length, prot, file->node, offset, &error);
    return mapped ? (int64_t)mapped : error;
}

// munmap(addr, length): give back mapped memory, its frames and its promise
static int64_t sys_munmap(uint64_t addr, size_t length) {
    if (addr < USER_MMAP_BASE) return -EINVAL; // Not something mmap handed out
    return region_unmap(sched_get_current_task(), addr, length);
}

// sleep(milliseconds): sleep that long (at most a day). -EINTR if a signal
// handler cut it short.
static int64_t sys_sleep(uint64_t milliseconds) {
    if (milliseconds > 24ULL * 60 * 60 * 1000) return -EINVAL;
    return nanosleep(milliseconds * 1000000ULL) ? 0 : -EINTR; // To the millisecond, not to the next tick
}

// Copy a NULL-terminated vector of strings (argv or envp) from user memory.
// The strings go into `storage` starting at *used; out[] receives pointers
// to them and a final NULL. Returns the count or a negative error number.
static int64_t copy_string_vector(uint64_t user_vec, char *storage, size_t *used, char *out[]) {
    size_t count = 0;

    for (; user_vec != 0; count++) {
        uint64_t slot = user_vec + count * sizeof(uint64_t);
        if (!user_range_ok(slot, sizeof(uint64_t), false)) return -EFAULT;

        user_access_begin();
        uint64_t user_string = *(const uint64_t *)slot;
        user_access_end();
        if (user_string == 0) break;

        if (count >= PROCESS_MAX_ARGS) return -E2BIG;
        if (*used >= PROCESS_ARG_BYTES) return -E2BIG;

        int64_t len = copy_string_from_user(storage + *used, user_string, PROCESS_ARG_BYTES - *used);
        if (len < 0) {
            // Unreadable, or it did not fit in what is left of the storage
            return user_range_ok(user_string, 1, false) ? -E2BIG : -EFAULT;
        }
        out[count] = storage + *used;
        *used += (size_t)len + 1;
    }

    out[count] = NULL;
    return (int64_t)count;
}

// execve(path, argv, envp): replace the caller's program with the one at
// path (permissions checked: execute and read). The arguments and
// environment are copied in first, since the address space they live in
// is about to go. Does not return on success; -EACCES, -ENOENT (not found
// or not a valid program), -E2BIG, -EFAULT.
static int64_t sys_execve(struct registers *regs, uint64_t user_path, uint64_t user_argv, uint64_t user_envp) {
    // Copy the path, arguments and environment into kernel memory first:
    // exec destroys the address space they live in.
    char path[PATH_MAX];
    char *argv[PROCESS_MAX_ARGS + 1];
    char *envp[PROCESS_MAX_ARGS + 1];

    int64_t path_len = copy_string_from_user(path, user_path, sizeof(path));
    if (path_len < 0) return path_len;

    char *storage = kmalloc(PROCESS_ARG_BYTES);
    if (!storage) return -ENOMEM;

    size_t used = 0;
    int64_t argc = copy_string_vector(user_argv, storage, &used, argv);
    int64_t envc = (argc >= 0) ? copy_string_vector(user_envp, storage, &used, envp) : 0;

    int64_t result;
    if (argc < 0) {
        result = argc;
    } else if (envc < 0) {
        result = envc;
    } else {
        // On success the frame describes the new program, and the 0 returned
        // here lands in its RAX, which a new program does not look at.
        // (Looked at here only to tell the caller *why*: exec checks again
        // itself, under the same lock as the loading.)
        vfs_lock();
        vnode_t *program = vfs_find(sched_get_current_task()->cwd, path);
        bool refused = program && program->type == VFS_FILE && !auth_may_execute(auth_current_uid(), program);
        vfs_unlock();
        if (refused) result = -EACCES;
        else result = (process_exec(regs, path, argc ? argv : NULL, envp) == 0) ? 0 : -ENOENT;
    }

    kfree(storage);
    return result;
}

// waitpid(pid, status, options): see process_wait_options. Stores the
// status if a pointer is given. -ECHILD if there is no such child, -EINTR
// if a signal interrupted the wait.
static int64_t sys_waitpid(int pid, uint64_t user_status, uint64_t options) {
    if (options & ~(uint64_t)(PROCESS_WNOHANG | PROCESS_WUNTRACED)) return -EINVAL;
    if (user_status != 0 && !user_range_ok(user_status, sizeof(int), true)) return -EFAULT;

    int status = 0;
    pid_t child = process_wait_options(pid, &status, (int)options);
    if (child == PROCESS_WAIT_INTERRUPTED) return -EINTR;
    if (child < 0) return -ECHILD;
    if (child == 0) return 0; // WNOHANG: nobody has exited yet

    if (user_status != 0) {
        user_access_begin();
        *(int *)user_status = status;
        user_access_end();
    }
    return child;
}

// uname(buf): the system's name, release and machine, into *buf
static int64_t sys_uname(uint64_t user_buf) {
    if (!user_range_ok(user_buf, sizeof(struct utsname), true)) return -EFAULT;

    struct utsname *buf = (struct utsname *)user_buf;
    user_access_begin();
    memset(buf, 0, sizeof(*buf));
    strncpy(buf->sysname, "BashketOS", 64);
    strncpy(buf->nodename, "bashket", 64);
    strncpy(buf->release, "0.1.0", 64);
    strncpy(buf->version, "BashketKernel", 64);
    strncpy(buf->machine, "x86_64", 64);
    user_access_end();
    return 0;
}

// getcwd(buf, size): the caller's working directory as an absolute path.
// Returns its length; -EINVAL if buf is too small.
static int64_t sys_getcwd(uint64_t user_buf, size_t size) {
    char path[PATH_MAX];
    task_t *task = sched_get_current_task();
    size_t len = vfs_path_of(task->cwd ? task->cwd : vfs_get_root(), path, sizeof(path));
    if (len == 0) return -EINVAL;

    if (size < len + 1) return -EINVAL; // The caller's buffer is too small
    if (!user_range_ok(user_buf, len + 1, true)) return -EFAULT;

    user_access_begin();
    memcpy((void *)user_buf, path, len + 1);
    user_access_end();
    return (int64_t)len;
}

// chdir(path): change the caller's working directory
static int64_t sys_chdir(uint64_t user_path) {
    char path[PATH_MAX];
    int64_t path_len = copy_string_from_user(path, user_path, sizeof(path));
    if (path_len < 0) return path_len;

    task_t *task = sched_get_current_task();
    vnode_t *dir = vfs_find(task->cwd, path);
    if (!dir) return -ENOENT;
    if (dir->type != VFS_DIRECTORY) return -ENOTDIR;

    task_set_cwd(task, dir == vfs_get_root() ? NULL : dir);
    return 0;
}

// --- Shared memory segments ---------------------------------------------------

static int64_t sys_shmat(int id) {
    task_t *task = sched_get_current_task();
    uint64_t addr = shm_attach(id, current_space(), &task->mmap_next);
    return addr ? (int64_t)addr : -EINVAL;
}

// --- Network -----------------------------------------------------------------

static uint64_t ms_to_ticks(uint64_t ms) {
    if (ms > 3600 * 1000) ms = 3600 * 1000; // An hour is plenty, and keeps the arithmetic small
    return (ms * TIMER_HZ + 999) / 1000;
}

// The UDP socket behind descriptor fd, or NULL if it is not one
static udp_socket_t *fd_to_socket(int fd) {
    file_t *file = fd_to_file(fd);
    return (file && file->kind == FILE_SOCKET) ? file->socket : NULL;
}

// The TCP socket behind descriptor fd, or NULL if it is not one
static tcp_socket_t *fd_to_tcp(int fd) {
    file_t *file = fd_to_file(fd);
    return (file && file->kind == FILE_TCP) ? file->tcp : NULL;
}

// socket(type): a new UDP (SOCK_DGRAM) or TCP (SOCK_STREAM) socket, as a
// descriptor. -ENETDOWN without a network card.
static int64_t sys_socket(uint64_t type) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EINVAL;
    if (!net_up()) return -ENETDOWN;
    file_t *file = (type == SOCK_STREAM) ? file_open_tcp(NULL) : file_open_socket();
    if (!file) return -ENOMEM;

    int fd = fd_install(sched_get_current_task()->fds, file);
    if (fd < 0) file_close(file);
    return fd;
}

// connect(fd, ip, port): connect a TCP socket; waits until the connection
// is made or refused
static int64_t sys_connect(int fd, uint64_t ip, uint64_t port) {
    tcp_socket_t *socket = fd_to_tcp(fd);
    if (!socket) return -EBADF;
    if (ip == 0 || ip > 0xFFFFFFFFULL || port == 0 || port > 0xFFFF) return -EINVAL;
    return tcp_connect(socket, (uint32_t)ip, (uint16_t)port);
}

// accept(fd, from): wait for a connection on a listening TCP socket and
// return it as a new descriptor; the peer's address goes to *from if given
static int64_t sys_accept(int fd, uint64_t user_from) {
    tcp_socket_t *listener = fd_to_tcp(fd);
    if (!listener) return -EBADF;
    if (user_from != 0 && !user_range_ok(user_from, sizeof(struct net_address), true)) return -EFAULT;

    // Make sure there is a descriptor to give the connection before taking
    // it: once accepted it could only be dropped
    task_t *task = sched_get_current_task();
    bool have_slot = false;
    for (int i = 0; i < TASK_MAX_FDS; i++) if (!task->fds[i]) have_slot = true;
    if (!have_slot) return -EMFILE;

    int error = 0;
    tcp_socket_t *connection = tcp_accept(listener, &error);
    if (!connection) return error;

    struct net_address from = {0};
    tcp_peer(connection, &from.ip, &from.port);

    file_t *file = file_open_tcp(connection); // Closes the connection if it fails
    if (!file) return -ENOMEM;
    int new_fd = fd_install(task->fds, file);
    if (new_fd < 0) {
        file_close(file);
        return new_fd;
    }

    if (user_from != 0) {
        user_access_begin();
        memcpy((void *)user_from, &from, sizeof(from));
        user_access_end();
    }
    return new_fd;
}

// sendto(fd, buf, len, ip, port): send one UDP datagram
static int64_t sys_sendto(int fd, uint64_t user_buf, size_t len, uint32_t ip, uint64_t port) {
    udp_socket_t *socket = fd_to_socket(fd);
    if (!socket) return -EBADF;
    if (len > UDP_PAYLOAD_MAX) return -EMSGSIZE;
    if (port == 0 || port > 0xFFFF) return -EINVAL;
    if (len > 0 && !user_range_ok(user_buf, len, false)) return -EFAULT;

    // Sending can sleep (waiting for an ARP answer), so the datagram is
    // copied into kernel memory first and the user's buffer is left alone
    uint8_t data[UDP_PAYLOAD_MAX];
    user_access_begin();
    memcpy(data, (const void *)user_buf, len);
    user_access_end();

    return udp_sendto(socket, ip, (uint16_t)port, data, len);
}

// poll(fds, count, timeout_ms): sleep until one of the descriptors is ready
// (poll.h). The array is copied in, worked on in kernel memory, and the
// results copied back: nothing below this call sees a user pointer.
static int64_t sys_poll(uint64_t user_fds, uint64_t count, int64_t timeout_ms) {
    if (count > POLL_MAX) return -EINVAL;
    size_t bytes = (size_t)count * sizeof(struct pollfd);
    if (count > 0 && !user_range_ok(user_fds, bytes, true)) return -EFAULT;

    struct pollfd fds[POLL_MAX];
    user_access_begin();
    memcpy(fds, (const void *)user_fds, bytes);
    user_access_end();

    int64_t result = poll_wait(sched_get_current_task()->fds, fds, (size_t)count, timeout_ms);
    if (result < 0) return result;

    if (count > 0 && !user_range_ok(user_fds, bytes, true)) return -EFAULT; // (It may have been unmapped meanwhile)
    user_access_begin();
    for (size_t i = 0; i < count; i++) ((struct pollfd *)user_fds)[i].revents = fds[i].revents;
    user_access_end();
    return result;
}

// recvfrom(fd, buf, len, from, timeout_ms): receive one UDP datagram,
// waiting up to timeout_ms (0: as long as it takes). Returns its length;
// the sender's address goes to *from if given.
static int64_t sys_recvfrom(int fd, uint64_t user_buf, size_t len, uint64_t user_from, uint64_t timeout_ms) {
    udp_socket_t *socket = fd_to_socket(fd);
    if (!socket) return -EBADF;
    if (len > UDP_PAYLOAD_MAX) len = UDP_PAYLOAD_MAX; // No datagram is longer
    if (len > 0 && !user_range_ok(user_buf, len, true)) return -EFAULT;
    if (user_from != 0 && !user_range_ok(user_from, sizeof(struct net_address), true)) return -EFAULT;

    uint8_t data[UDP_PAYLOAD_MAX];
    struct net_address from = {0};
    int64_t n = udp_recvfrom(socket, data, len, &from.ip, &from.port, ms_to_ticks(timeout_ms));
    if (n < 0) return n;

    user_access_begin();
    memcpy((void *)user_buf, data, (size_t)n);
    if (user_from != 0) memcpy((void *)user_from, &from, sizeof(from));
    user_access_end();
    return n;
}

// netconfig(get, set): copy the network settings to *get, and (root only)
// replace them with *set; either may be NULL
static int64_t sys_netconfig(uint64_t user_get, uint64_t user_set) {
    if (user_get != 0 && !user_range_ok(user_get, sizeof(net_config_t), true)) return -EFAULT;
    if (user_set != 0 && !user_range_ok(user_set, sizeof(net_config_t), false)) return -EFAULT;
    if (!net_up()) return -ENETDOWN;

    // Changing the machine's address is the administrator's business
    if (user_set != 0 && sched_get_current_task()->uid != UID_ROOT) return -EPERM;

    net_config_t config;
    if (user_get != 0) {
        net_get_config(&config);
        user_access_begin();
        memcpy((void *)user_get, &config, sizeof(config));
        user_access_end();
    }
    if (user_set != 0) {
        user_access_begin();
        memcpy(&config, (const void *)user_set, sizeof(config));
        user_access_end();
        return net_set_config(&config);
    }
    return 0;
}

// Returns the round-trip time in microseconds
static int64_t sys_ping(uint32_t ip, uint64_t timeout_ms) {
    uint64_t start = clock_ns();
    int err = net_ping(ip, ms_to_ticks(timeout_ms), NULL);
    return err != 0 ? err : (int64_t)((clock_ns() - start) / 1000);
}

// --- Dispatcher --------------------------------------------------------------

static int64_t dispatch(struct registers *regs);

// The calls that can simply be made again if a signal interrupts them
// before they have done anything
static bool restartable(uint64_t number) {
    return number == SYS_READ || number == SYS_WRITE || number == SYS_WAITPID || number == SYS_ACCEPT;
}

// sigaction(sig, new, old, restorer): read and/or set what happens on
// signal `sig`; `restorer` is the user function that calls sigreturn when
// a handler returns
static int64_t sys_sigaction(int sig, uint64_t user_new, uint64_t user_old, uint64_t restorer) {
    if (user_new != 0 && !user_range_ok(user_new, sizeof(struct signal_action), false)) return -EFAULT;
    if (user_old != 0 && !user_range_ok(user_old, sizeof(struct signal_action), true)) return -EFAULT;

    struct signal_action new_action, old_action;
    if (user_new != 0) {
        user_access_begin();
        memcpy(&new_action, (const void *)user_new, sizeof(new_action));
        user_access_end();
    }
    int err = signal_set_action(sched_get_current_task(), sig, user_new ? &new_action : NULL, restorer, &old_action);
    if (err == 0 && user_old != 0) {
        user_access_begin();
        memcpy((void *)user_old, &old_action, sizeof(old_action));
        user_access_end();
    }
    return err;
}

// sigprocmask(how, set, old): change which signals are held back
// (SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK) and/or read the mask as it was
static int64_t sys_sigprocmask(int how, uint64_t user_set, uint64_t user_old) {
    if (user_set != 0 && !user_range_ok(user_set, sizeof(uint32_t), false)) return -EFAULT;
    if (user_old != 0 && !user_range_ok(user_old, sizeof(uint32_t), true)) return -EFAULT;

    task_t *task = sched_get_current_task();
    uint32_t set = 0, old = task->sig_blocked;
    int err = 0;
    if (user_set != 0) {
        user_access_begin();
        memcpy(&set, (const void *)user_set, sizeof(set));
        user_access_end();
        err = signal_change_mask(task, how, set, &old);
    }
    if (err == 0 && user_old != 0) {
        user_access_begin();
        memcpy((void *)user_old, &old, sizeof(old));
        user_access_end();
    }
    return err;
}

int64_t syscall_handler(struct registers *regs) {
    uint64_t number = regs->rax;
    int64_t result = dispatch(regs);

    // If a signal handler is about to run instead of this call's result
    // being looked at, the handler's settings may ask for the call to be
    // made again (see signal_deliver)
    task_t *caller = sched_get_current_task();
    if (caller) caller->restart_syscall = (result == -EINTR && restartable(number)) ? (uint32_t)number + 1 : 0;

    // About to return to user code: the system call is complete and nothing
    // is held, so a pending kill can take effect here.
    sched_check_kill();

    // The return to ring 3 uses sysretq, which takes the address from the
    // frame. If that address is not canonical (a `syscall` in the last two
    // bytes of user space returns to 0x0000800000000000), an Intel CPU raises
    // the resulting fault in ring 0 but on the user's stack, which hands
    // control of the kernel to the program (CVE-2012-0217). AMD CPUs and QEMU
    // fault after the switch to ring 3 instead, so this cannot be seen under
    // QEMU; the check is here for real Intel hardware.
    task_t *task = sched_get_current_task();
    if (regs->rip >= USER_SPACE_END && task && task->is_user) {
        kprintf("\n[pid %u %s] killed: system call would return to invalid address 0x%lx\n",
                task->pid, task->name, regs->rip);
        task_exit_status(128 + SIGSEGV);
    }

    // sigreturn replaced the whole frame, RAX included: leave it alone. The
    // entry stub sees int_no and restores the frame with iretq.
    // A signal that was held back while the handler ran is due now: its
    // handler is entered straight from here, on top of the restored state.
    if (regs->int_no == SYSCALL_RETURN_FULL_FRAME) {
        signal_deliver(regs);
        return (int64_t)regs->rax;
    }

    // If a signal handler has to run, the frame saved for it must show this
    // system call as finished, with its result in RAX.
    regs->rax = (uint64_t)result;
    signal_deliver(regs);
    return (int64_t)regs->rax;
}

// Call the function for system call number regs->rax with the arguments in
// rdi, rsi, rdx, r10, r8, r9 (the System V convention, with r10 for rcx,
// which syscall overwrites). Returns its result: a value, or a negative
// error number (-ENOSYS for an unknown call).
static int64_t dispatch(struct registers *regs) {
    task_t *task = sched_get_current_task();

    switch (regs->rax) {
        case SYS_READ:    return sys_read((int)regs->rdi, regs->rsi, (size_t)regs->rdx);
        case SYS_WRITE:   return sys_write((int)regs->rdi, regs->rsi, (size_t)regs->rdx);

        case SYS_YIELD:
            sched_yield();
            return 0;

        case SYS_EXIT:
            task_exit_status((int)regs->rdi & 0xFF);
            return 0; // Not reached

        case SYS_GETPID:
        case SYS_GETPID_ALT:
            return (int64_t)task->pid;
        case SYS_GETPPID:
            return (int64_t)task->parent_pid;
        case SYS_SETPGID:
            if (regs->rdi > 0x7FFFFFFF || regs->rsi > 0x7FFFFFFF) return -EINVAL;
            return process_set_group((pid_t)regs->rdi, (uint32_t)regs->rsi) == 0 ? 0 : -EPERM;
        case SYS_GETPGID: {
            if (regs->rdi == 0) return (int64_t)task->pgid;
            uint64_t irq = sched_lock_irqsave();
            task_t *other = sched_find_task_locked((uint32_t)regs->rdi);
            int64_t pgid = (other && other->is_user) ? (int64_t)other->pgid : -ESRCH;
            sched_unlock_irqrestore(irq);
            return pgid;
        }
        case SYS_TCSETPGRP:
            // Whoever reads the terminal may say which of its jobs has it
            if (regs->rdi > 0x7FFFFFFF) return -EINVAL;
            return process_set_foreground((uint32_t)regs->rdi) == 0 ? 0 : -EINVAL;
        case SYS_TCGETPGRP:
            return (int64_t)process_get_foreground();

        case SYS_BRK:     return sys_brk(regs->rdi);
        case SYS_MMAP:    return sys_mmap(regs->rdi, (size_t)regs->rsi, (int)regs->rdx, (int)regs->r10, (int)regs->r8, regs->r9);
        case SYS_MUNMAP:  return sys_munmap(regs->rdi, (size_t)regs->rsi);
        case SYS_SLEEP:   return sys_sleep(regs->rdi);
        case SYS_TIME:    return (int64_t)timer_ticks();
        case SYS_CLOCK_NS: return (int64_t)clock_ns();
        case SYS_REALTIME: return (int64_t)walltime_ns();
        case SYS_SETTIME:
            // Every process's clock: root's alone to change
            if (sched_get_current_task()->uid != UID_ROOT) return -EPERM;
            return walltime_set(regs->rdi);
        case SYS_NANOSLEEP:
            if (regs->rdi > 3600ULL * 1000000000ULL) return -EINVAL;
            return nanosleep(regs->rdi) ? 0 : -EINTR;

        case SYS_FORK: {
            pid_t child = process_fork(regs);
            return child < 0 ? -ENOMEM : child;
        }
        case SYS_EXECVE:  return sys_execve(regs, regs->rdi, regs->rsi, regs->rdx);
        case SYS_WAITPID: return sys_waitpid((int)regs->rdi, regs->rsi, regs->rdx);
        case SYS_KILL:    return sys_kill((int)regs->rdi, (int)regs->rsi);

        case SYS_GETUID:  return (int64_t)task->ruid;   // Who started this process
        case SYS_GETEUID: return (int64_t)task->uid;    // Whose rights it has
        case SYS_GETGID:  return (int64_t)task->gid;
        case SYS_SETUID:
            // Root may become anyone, for good: real and effective user both
            // change. Anyone else may only go back to being themselves, which
            // is how a set-uid program gives up the rights it was lent.
            if (regs->rdi > 0xFFFFFFFFULL) return -EINVAL;
            if (task->uid == UID_ROOT) {
                task->ruid = (uint32_t)regs->rdi;
            } else if ((uint32_t)regs->rdi != task->ruid) {
                return -EPERM;
            }
            task->uid = (uint32_t)regs->rdi;
            return 0;
        case SYS_SETGID:
            if (regs->rdi > 0xFFFFFFFFULL) return -EINVAL;
            if (task->uid != UID_ROOT && (uint32_t)regs->rdi != task->gid) return -EPERM;
            task->gid = (uint32_t)regs->rdi;
            return 0;
        case SYS_UMASK: {
            uint16_t previous = task->umask;
            task->umask = (uint16_t)(regs->rdi & 0777);
            return previous;
        }
        case SYS_RENAME: { vfs_lock(); int64_t r = sys_rename(regs->rdi, regs->rsi); vfs_unlock(); return r; }
        case SYS_CHMOD: { vfs_lock(); int64_t r = sys_chmod(regs->rdi, regs->rsi); vfs_unlock(); return r; } // Look up, then act: one step
        case SYS_CHOWN: { vfs_lock(); int64_t r = sys_chown(regs->rdi, regs->rsi, regs->rdx); vfs_unlock(); return r; } // Look up, then act: one step

        case SYS_SETPRIORITY:
            if (regs->rdi > PRIO_LOW) return -EINVAL;
            task_set_priority(task, (uint8_t)regs->rdi);
            return 0;

        // A segment belongs to the user who created it. Someone else's is
        // not found by its key, and its number is of no use to them.
        case SYS_SHMGET: {
            int id = shm_get((uint32_t)regs->rdi, (size_t)regs->rsi);
            return (id >= 0 && !shm_may_use(id, task->uid)) ? -EACCES : id;
        }
        case SYS_SHMAT:
            if (shm_count_valid((int)regs->rdi) && !shm_may_use((int)regs->rdi, task->uid)) return -EACCES;
            return sys_shmat((int)regs->rdi);
        case SYS_SHMRM:
            if (shm_count_valid((int)regs->rdi) && !shm_may_use((int)regs->rdi, task->uid)) return -EACCES;
            return shm_remove((int)regs->rdi);

        case SYS_SOCKET:   return sys_socket(regs->rdi);
        case SYS_CONNECT:  return sys_connect((int)regs->rdi, regs->rsi, regs->rdx);
        case SYS_ACCEPT:   return sys_accept((int)regs->rdi, regs->rsi);
        case SYS_SHUTDOWN: {
            tcp_socket_t *stream = fd_to_tcp((int)regs->rdi);
            return stream ? tcp_shutdown(stream) : -EBADF;
        }
        case SYS_LISTEN: {
            tcp_socket_t *listener = fd_to_tcp((int)regs->rdi);
            return listener ? tcp_listen(listener, (int)regs->rsi) : -EBADF;
        }
        case SYS_BIND: {
            udp_socket_t *socket = fd_to_socket((int)regs->rdi);
            tcp_socket_t *stream = fd_to_tcp((int)regs->rdi);
            if (!socket && !stream) return -EBADF;
            if (regs->rsi == 0 || regs->rsi > 0xFFFF) return -EINVAL;
            // Ports below 1024 are where system services live: root only
            if (regs->rsi < 1024 && task->uid != UID_ROOT) return -EACCES;
            if (stream) return tcp_bind(stream, (uint16_t)regs->rsi);
            return udp_bind(socket, (uint16_t)regs->rsi);
        }
        case SYS_SENDTO:   return sys_sendto((int)regs->rdi, regs->rsi, (size_t)regs->rdx, (uint32_t)regs->r10, regs->r8);
        case SYS_RECVFROM: return sys_recvfrom((int)regs->rdi, regs->rsi, (size_t)regs->rdx, regs->r10, regs->r8);
        case SYS_PING:     return sys_ping((uint32_t)regs->rdi, regs->rsi);
        case SYS_NETCONFIG: return sys_netconfig(regs->rdi, regs->rsi);

        case SYS_TTY_RAW:
            tty_set_raw(regs->rdi != 0, task->pid);
            return 0;
        case SYS_REBOOT:
            // Only root may stop the system (rdi: 0 switch off, 1 restart)
            if (task->uid != UID_ROOT) return -EPERM;
            if (regs->rdi > 1) return -EINVAL;
            system_shutdown(regs->rdi == 1);

        case SYS_FB_INFO: {
            // Width, height, bytes per row, bits per pixel of /dev/fb0
            uint32_t info[4] = { 0, 0, 0, 32 };
            if (!user_range_ok(regs->rdi, sizeof(info), true)) return -EFAULT;
            if (!console_framebuffer(NULL, &info[0], &info[1], &info[2])) return -ENODEV;
            user_access_begin();
            memcpy((void *)regs->rdi, info, sizeof(info));
            user_access_end();
            return 0;
        }
        case SYS_TTY_SIZE: {
            if (!user_range_ok(regs->rdi, 2 * sizeof(uint32_t), true)) return -EFAULT;
            uint32_t size[2];
            console_get_size(&size[0], &size[1]);
            user_access_begin();
            memcpy((void *)regs->rdi, size, sizeof(size));
            user_access_end();
            return 0;
        }

        case SYS_SIGNAL:
            return signal_set_handler(task, (int)regs->rdi, regs->rsi, regs->rdx);
        case SYS_SIGACTION:   return sys_sigaction((int)regs->rdi, regs->rsi, regs->rdx, regs->r10);
        case SYS_SIGPROCMASK: return sys_sigprocmask((int)regs->rdi, regs->rsi, regs->rdx);
        case SYS_SIGRETURN:
            signal_return(regs);
            return 0; // Ignored: the frame's own RAX is what the program gets

        case SYS_UNAME:   return sys_uname(regs->rdi);
        case SYS_GETCWD: { vfs_lock(); int64_t r = sys_getcwd(regs->rdi, (size_t)regs->rsi); vfs_unlock(); return r; } // Look up, then act: one step
        case SYS_CHDIR: { vfs_lock(); int64_t r = sys_chdir(regs->rdi); vfs_unlock(); return r; } // Look up, then act: one step

        case SYS_CLEAR:
            kprintf("\033[2J\033[H"); // Erase the screen, cursor to the top left
            return 0;

        case SYS_OPEN:    return sys_open(regs->rdi, (int)regs->rsi);
        case SYS_CLOSE:   return sys_close((int)regs->rdi);
        case SYS_LSEEK: {
            file_t *file = fd_to_file((int)regs->rdi);
            return file ? file_seek(file, (int64_t)regs->rsi, (int)regs->rdx) : -EBADF;
        }
        case SYS_PIPE:    return sys_pipe(regs->rdi, 0);
        case SYS_PIPE2:   return sys_pipe(regs->rdi, regs->rsi);
        case SYS_POLL:    return sys_poll(regs->rdi, regs->rsi, (int64_t)(int32_t)regs->rdx);
        case SYS_DUP:     return sys_dup((int)regs->rdi);
        case SYS_DUP2:    return sys_dup2((int)regs->rdi, (int)regs->rsi);
        case SYS_FCNTL:   return sys_fcntl((int)regs->rdi, regs->rsi, regs->rdx);
        case SYS_READDIR: return sys_readdir((int)regs->rdi, regs->rsi);
        case SYS_MKDIR: { vfs_lock(); int64_t r = sys_mkdir(regs->rdi); vfs_unlock(); return r; } // Look up, then act: one step
        case SYS_UNLINK: { vfs_lock(); int64_t r = sys_unlink(regs->rdi); vfs_unlock(); return r; } // Look up, then act: one step

        default:
            return -ENOSYS;
    }
}
