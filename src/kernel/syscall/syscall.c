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

extern void syscall_entry(void);

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_AC (1ULL << 18)

#define PATH_MAX 128

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init(void) {
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

    kprintf("Syscall Init: MSRs configured for syscall/sysret (LSTAR: %p)\n", syscall_entry);
}

// --- User memory access -----------------------------------------------------

static uint64_t *current_space(void) {
    task_t *task = sched_get_current_task();
    return vmm_space_from_phys((uint64_t)task->pml4_phys);
}

static bool user_range_ok(uint64_t addr, size_t len, bool write) {
    return vmm_user_range_ok(current_space(), addr, len, write);
}

// Copy a NUL-terminated string from user memory into a kernel buffer.
// Returns its length, or -EFAULT if it is unreadable or does not fit.
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
    return -EFAULT; // No terminator within dst_size
}

// --- Individual system calls -------------------------------------------------

// Look up a descriptor in the calling process's table
static file_t *fd_to_file(int fd) {
    task_t *task = sched_get_current_task();
    if (fd < 0 || fd >= TASK_MAX_FDS) return NULL;
    return task->fds[fd];
}

static int64_t sys_read(int fd, uint64_t buf, size_t count) {
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    if (count == 0) return 0;
    if (!user_range_ok(buf, count, true)) return -EFAULT;

    // May sleep (console, pipe). The caller's address space is loaded again
    // whenever it runs, so writing to its buffer afterwards is fine.
    user_access_begin();
    int64_t result = file_read(file, (void *)buf, count);
    user_access_end();
    return result;
}

static int64_t sys_write(int fd, uint64_t buf, size_t count) {
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    if (count == 0) return 0;
    if (!user_range_ok(buf, count, false)) return -EFAULT;

    user_access_begin();
    int64_t result = file_write(file, (const void *)buf, count);
    user_access_end();
    return result;
}

static int64_t sys_open(uint64_t user_path, int flags) {
    char path[PATH_MAX];
    if (copy_string_from_user(path, user_path, sizeof(path)) < 0) return -EFAULT;

    file_t *file = NULL;
    int err = file_open_at(sched_get_current_task()->cwd, path, flags, &file);
    if (err != 0) return err;

    int fd = fd_install(sched_get_current_task()->fds, file);
    if (fd < 0) file_close(file);
    return fd;
}

static int64_t sys_close(int fd) {
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;

    sched_get_current_task()->fds[fd] = NULL;
    file_close(file);
    return 0;
}

static int64_t sys_pipe(uint64_t user_fds) {
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

    int *out = (int *)user_fds;
    user_access_begin();
    out[0] = read_fd;
    out[1] = write_fd;
    user_access_end();
    return 0;
}

static int64_t sys_dup2(int old_fd, int new_fd) {
    file_t *file = fd_to_file(old_fd);
    if (!file || new_fd < 0 || new_fd >= TASK_MAX_FDS) return -EBADF;
    if (old_fd == new_fd) return new_fd;

    task_t *task = sched_get_current_task();
    file_t *replaced = task->fds[new_fd];
    task->fds[new_fd] = file_dup(file);
    file_close(replaced);
    return new_fd;
}

static int64_t sys_readdir(int fd, uint64_t user_entry) {
    file_t *file = fd_to_file(fd);
    if (!file) return -EBADF;
    if (!user_range_ok(user_entry, sizeof(struct dirent_info), true)) return -EFAULT;

    user_access_begin();
    int result = file_readdir(file, (struct dirent_info *)user_entry);
    user_access_end();
    return result;
}

static int64_t sys_mkdir(uint64_t user_path) {
    char path[PATH_MAX];
    if (copy_string_from_user(path, user_path, sizeof(path)) < 0) return -EFAULT;

    const char *leaf = NULL;
    vnode_t *parent = vfs_find_parent_at(sched_get_current_task()->cwd, path, &leaf);
    if (!parent) return -ENOENT;

    // Copy the leaf without a trailing slash ("mkdir /tmp/dir/")
    char name[PATH_MAX];
    size_t len = 0;
    while (leaf[len] != '\0' && leaf[len] != '/') { name[len] = leaf[len]; len++; }
    name[len] = '\0';

    return vfs_create(parent, name, VFS_DIRECTORY, NULL);
}

static int64_t sys_unlink(uint64_t user_path) {
    char path[PATH_MAX];
    if (copy_string_from_user(path, user_path, sizeof(path)) < 0) return -EFAULT;

    const char *leaf = NULL;
    vnode_t *parent = vfs_find_parent_at(sched_get_current_task()->cwd, path, &leaf);
    if (!parent) return -ENOENT;
    return vfs_unlink(parent, leaf);
}

static int64_t sys_brk(uint64_t new_brk) {
    task_t *task = sched_get_current_task();
    if (new_brk == 0) return (int64_t)task->brk_end;
    return (int64_t)user_set_brk(current_space(), task->brk_start, &task->brk_end, new_brk);
}

static int64_t sys_mmap(uint64_t addr, size_t length, int prot, int flags) {
    // Only private anonymous memory at a kernel-chosen address is supported
    if (addr != 0 || !(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED)) return -EINVAL;
    if (length == 0) return -EINVAL;

    task_t *task = sched_get_current_task();
    uint64_t mapped = user_mmap_anon(current_space(), &task->mmap_next, length, prot);
    return mapped ? (int64_t)mapped : -ENOMEM;
}

static int64_t sys_munmap(uint64_t addr, size_t length) {
    if (addr < USER_MMAP_BASE) return -EINVAL; // Not something mmap handed out
    return user_unmap_range(current_space(), addr, length) == 0 ? 0 : -EINVAL;
}

static int64_t sys_sleep(uint64_t milliseconds) {
    if (milliseconds > 24ULL * 60 * 60 * 1000) return -EINVAL;
    return sched_sleep_ticks((milliseconds * TIMER_HZ + 999) / 1000) ? 0 : -EINTR;
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

static int64_t sys_execve(struct registers *regs, uint64_t user_path, uint64_t user_argv, uint64_t user_envp) {
    // Copy the path, arguments and environment into kernel memory first:
    // exec destroys the address space they live in.
    char path[PATH_MAX];
    char *argv[PROCESS_MAX_ARGS + 1];
    char *envp[PROCESS_MAX_ARGS + 1];

    if (copy_string_from_user(path, user_path, sizeof(path)) < 0) return -EFAULT;

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
        result = (process_exec(regs, path, argc ? argv : NULL, envp) == 0) ? 0 : -ENOENT;
    }

    kfree(storage);
    return result;
}

static int64_t sys_waitpid(int pid, uint64_t user_status) {
    if (user_status != 0 && !user_range_ok(user_status, sizeof(int), true)) return -EFAULT;

    int status = 0;
    pid_t child = process_wait(pid, &status);
    if (child == PROCESS_WAIT_INTERRUPTED) return -EINTR;
    if (child < 0) return -ECHILD;

    if (user_status != 0) {
        user_access_begin();
        *(int *)user_status = status;
        user_access_end();
    }
    return child;
}

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

static int64_t sys_chdir(uint64_t user_path) {
    char path[PATH_MAX];
    if (copy_string_from_user(path, user_path, sizeof(path)) < 0) return -EFAULT;

    task_t *task = sched_get_current_task();
    vnode_t *dir = vfs_find(task->cwd, path);
    if (!dir) return -ENOENT;
    if (dir->type != VFS_DIRECTORY) return -ENOTDIR;

    task_set_cwd(task, dir == vfs_get_root() ? NULL : dir);
    return 0;
}

// --- Network -----------------------------------------------------------------

static uint64_t ms_to_ticks(uint64_t ms) {
    if (ms > 3600 * 1000) ms = 3600 * 1000; // An hour is plenty, and keeps the arithmetic small
    return (ms * TIMER_HZ + 999) / 1000;
}

static udp_socket_t *fd_to_socket(int fd) {
    file_t *file = fd_to_file(fd);
    return (file && file->kind == FILE_SOCKET) ? file->socket : NULL;
}

static int64_t sys_socket(void) {
    if (!net_up()) return -ENETDOWN;
    file_t *file = file_open_socket();
    if (!file) return -ENOMEM;

    int fd = fd_install(sched_get_current_task()->fds, file);
    if (fd < 0) file_close(file);
    return fd;
}

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

static int64_t sys_ping(uint32_t ip, uint64_t timeout_ms) {
    uint64_t rtt_ticks = 0;
    int err = net_ping(ip, ms_to_ticks(timeout_ms), &rtt_ticks);
    return err != 0 ? err : (int64_t)(rtt_ticks * 1000 / TIMER_HZ);
}

// --- Dispatcher --------------------------------------------------------------

static int64_t dispatch(struct registers *regs);

int64_t syscall_handler(struct registers *regs) {
    int64_t result = dispatch(regs);

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
    if (regs->int_no == SYSCALL_RETURN_FULL_FRAME) return (int64_t)regs->rax;

    // If a signal handler has to run, the frame saved for it must show this
    // system call as finished, with its result in RAX.
    regs->rax = (uint64_t)result;
    signal_deliver(regs);
    return (int64_t)regs->rax;
}

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

        case SYS_BRK:     return sys_brk(regs->rdi);
        case SYS_MMAP:    return sys_mmap(regs->rdi, (size_t)regs->rsi, (int)regs->rdx, (int)regs->r10);
        case SYS_MUNMAP:  return sys_munmap(regs->rdi, (size_t)regs->rsi);
        case SYS_SLEEP:   return sys_sleep(regs->rdi);
        case SYS_TIME:    return (int64_t)pit_get_ticks();

        case SYS_FORK: {
            pid_t child = process_fork(regs);
            return child < 0 ? -ENOMEM : child;
        }
        case SYS_EXECVE:  return sys_execve(regs, regs->rdi, regs->rsi, regs->rdx);
        case SYS_WAITPID: return sys_waitpid((int)regs->rdi, regs->rsi);
        case SYS_KILL:
            return process_kill((pid_t)regs->rdi, (int)regs->rsi) == 0 ? 0 : -ESRCH;

        case SYS_SETPRIORITY:
            if (regs->rdi > PRIO_LOW) return -EINVAL;
            task_set_priority(task, (uint8_t)regs->rdi);
            return 0;

        case SYS_SOCKET:   return sys_socket();
        case SYS_BIND: {
            udp_socket_t *socket = fd_to_socket((int)regs->rdi);
            if (!socket) return -EBADF;
            return regs->rsi > 0xFFFF ? -EINVAL : udp_bind(socket, (uint16_t)regs->rsi);
        }
        case SYS_SENDTO:   return sys_sendto((int)regs->rdi, regs->rsi, (size_t)regs->rdx, (uint32_t)regs->r10, regs->r8);
        case SYS_RECVFROM: return sys_recvfrom((int)regs->rdi, regs->rsi, (size_t)regs->rdx, regs->r10, regs->r8);
        case SYS_PING:     return sys_ping((uint32_t)regs->rdi, regs->rsi);

        case SYS_TTY_RAW:
            tty_set_raw(regs->rdi != 0, task->pid);
            return 0;
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
        case SYS_SIGRETURN:
            signal_return(regs);
            return 0; // Ignored: the frame's own RAX is what the program gets

        case SYS_UNAME:   return sys_uname(regs->rdi);
        case SYS_GETCWD:  return sys_getcwd(regs->rdi, (size_t)regs->rsi);
        case SYS_CHDIR:   return sys_chdir(regs->rdi);

        case SYS_CLEAR:
            kprintf("\033[2J\033[H"); // Erase the screen, cursor to the top left
            return 0;

        case SYS_OPEN:    return sys_open(regs->rdi, (int)regs->rsi);
        case SYS_CLOSE:   return sys_close((int)regs->rdi);
        case SYS_LSEEK: {
            file_t *file = fd_to_file((int)regs->rdi);
            return file ? file_seek(file, (int64_t)regs->rsi, (int)regs->rdx) : -EBADF;
        }
        case SYS_PIPE:    return sys_pipe(regs->rdi);
        case SYS_DUP2:    return sys_dup2((int)regs->rdi, (int)regs->rsi);
        case SYS_READDIR: return sys_readdir((int)regs->rdi, regs->rsi);
        case SYS_MKDIR:   return sys_mkdir(regs->rdi);
        case SYS_UNLINK:  return sys_unlink(regs->rdi);

        default:
            return -ENOSYS;
    }
}
