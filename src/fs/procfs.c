/**
 * @file procfs.c
 * @brief /proc: kernel state presented as text files
 *
 * Nothing under /proc is stored. Each file has a generator function that
 * writes the current state as text; a read runs the generator and returns
 * the requested slice of what it produced.
 *
 * /proc/<pid>/ has a directory per task, with `status`, `cmdline`, `maps`
 * (what its address space holds) and `fd/` (a file per open descriptor,
 * which reads as what the descriptor refers to). They
 * are kept in step with the task list by the /proc directory's refresh
 * operation, which the VFS runs every time anything looks inside /proc: it
 * adds directories for new tasks and removes those of tasks that are gone
 * (unless someone still has one open, in which case it tries again next
 * time). The nodes hold only a pid; every read finds the task by that pid
 * under the scheduler lock, so a node never points at a freed task. A task
 * that has been reaped reads as ENOENT. /proc/self is a symbolic link
 * whose target is rewritten with the reader's pid at each refresh. `fd/`
 * is refreshed the same way, against the task's descriptor table.
 *
 * `maps` and `fd/` name files, so only the task's owner (and root) may
 * read them. Reading them looks at another task's memory regions and
 * descriptors, which that task changes without the scheduler lock; see
 * read_maps and read_fd for what makes that safe.
 */

#include "procfs.h"
#include "region.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "heap.h"
#include "pmm.h"
#include "pit.h"
#include "swap.h"
#include "sched.h"
#include "spinlock.h"
#include "kerrno.h"
#include "cpu.h"
#include "klog.h"
#include "mount.h"
#include "smp.h"
#include "file.h"
#include "mmap.h"
#include "process.h"
#include "interrupts.h"
#include "tcp.h"
#include "pcache.h"

#define PROC_TEXT_MAX 4096

// Writes a /proc file's whole current text into `buf` (at most `size`
// bytes, NUL-terminated) and returns its length. Must not sleep: some hold
// spinlocks while they format.
typedef size_t (*proc_generator_t)(char *buf, size_t size);

// Run `generate` into a temporary buffer and copy out `count` bytes from
// `offset` into the kernel buffer `buf`. Returns the bytes copied (0 past
// the end) or -ENOMEM. Text beyond PROC_TEXT_MAX is cut off.
static int64_t serve_text(proc_generator_t generate, size_t offset, void *buf, size_t count) {
    char *text = kmalloc(PROC_TEXT_MAX);
    if (!text) return -ENOMEM;

    size_t len = generate(text, PROC_TEXT_MAX);
    size_t n = 0;
    if (offset < len) {
        n = len - offset;
        if (n > count) n = count;
        memcpy(buf, text + offset, n);
    }

    kfree(text);
    return (int64_t)n;
}

// The generators: one per file, each named after it

// /proc/version: the kernel's name and version
static size_t gen_version(char *buf, size_t size) {
    return ksnprintf(buf, size, "BashketOS 0.1.0 x86_64\n");
}

// /proc/uptime: seconds since boot, and the tick count
static size_t gen_uptime(char *buf, size_t size) {
    uint64_t ticks = timer_ticks();
    uint64_t hundredths = (ticks % TIMER_HZ) * 100 / TIMER_HZ;
    return ksnprintf(buf, size, "%lu.%s%lu seconds (%lu ticks at %u Hz)\n",
                     ticks / TIMER_HZ, hundredths < 10 ? "0" : "", hundredths,
                     ticks, (unsigned)TIMER_HZ);
}

// /proc/meminfo: frames, promised pages, swap, the kernel heap and the page
// cache (whose pages mapped nowhere could be free at once: pcache.h)
static size_t gen_meminfo(char *buf, size_t size) {
    return ksnprintf(buf, size,
                     "MemTotal:   %lu kB\n"
                     "MemFree:    %lu kB\n"
                     "Promised:   %lu kB\n"
                     "SwapTotal:  %lu kB\n"
                     "SwapFree:   %lu kB\n"
                     "Swapped:    %lu pages out, %lu back in\n"
                     "KernelHeap: %lu bytes in use, %lu kB mapped\n"
                     "Refused:    %lu frame requests, %lu kernel heap requests\n"
                     "Cached:     %lu kB of file pages, %lu kB of them mapped nowhere\n",
                     (uint64_t)pmm_get_total_frames() * 4,
                     (uint64_t)pmm_get_free_frames() * 4,
                     (uint64_t)region_promised_pages() * 4,
                     (uint64_t)swap_total_pages() * 4,
                     (uint64_t)swap_free_pages() * 4,
                     swap_pages_out(), swap_pages_in(),
                     (uint64_t)kheap_get_allocated_bytes(),
                     (uint64_t)kheap_get_mapped_bytes() / 1024,
                     (uint64_t)pmm_get_failed_allocations(),
                     (uint64_t)kheap_get_failed_allocations(),
                     (uint64_t)pcache_pages() * 4,
                     (uint64_t)pcache_unmapped_pages() * 4);
}

// /proc/tasks: one line per task (taken under the scheduler lock)
static size_t gen_tasks(char *buf, size_t size) {
    static const char *const state_names[] = { "ready  ", "running", "blocked", "zombie ", "stopped" };
    size_t len = ksnprintf(buf, size, "PID\tPPID\tUID\tSTATE\tKIND\tPRIO\tTICKS\tCPU\tNAME\n");

    // The task list must not change under us. Formatting into a buffer
    // takes no other lock and cannot sleep.
    uint64_t flags = sched_lock_irqsave();
    for (task_t *t = sched_first_task(); t && len + 1 < size; t = t->next) {
        const char *state = (unsigned)t->state < 5 ? state_names[t->state] : "?";
        len += ksnprintf(buf + len, size - len, "%u\t%u\t%u\t%s\t%s\t%u\t%lu\t%u\t%s\n",
                         t->pid, t->parent_pid, t->uid, state, t->is_user ? "user" : "kernel",
                         (unsigned)t->priority, t->cpu_ticks, t->last_cpu, t->name);
    }
    sched_unlock_irqrestore(flags);
    return len;
}

// /proc/timers: the timer subsystem's own report (timer_describe)
static size_t gen_timers(char *buf, size_t size) {
    return timer_describe(buf, size);
}

// /proc/cpuinfo: CPUID's vendor, brand, family and model, CPUs, protections
static size_t gen_cpuinfo(char *buf, size_t size) {
    uint32_t r[4];

    // Leaf 0: the vendor string is in EBX, EDX, ECX (in that order)
    char vendor[13];
    cpuid(0, 0, r);
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = '\0';

    // Leaf 1: family and model, each with an "extended" part
    cpuid(1, 0, r);
    uint32_t family = (r[0] >> 8) & 0xF;
    uint32_t model = (r[0] >> 4) & 0xF;
    if (family == 0xF) family += (r[0] >> 20) & 0xFF;
    if (family >= 6) model |= ((r[0] >> 16) & 0xF) << 4;

    // Leaves 0x80000002..4: the 48-character brand string, if supported
    char brand[49] = "unknown";
    cpuid(0x80000000, 0, r);
    if (r[0] >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, r);
            memcpy(brand + i * 16, r, 16);
        }
        brand[48] = '\0';
    }

    uint64_t nmis = 0;
    for (uint32_t i = 0; i < smp_get_cpu_total(); i++) nmis += cpu_get(i)->nmis;

    return ksnprintf(buf, size,
                     "vendor_id:  %s\n"
                     "model name: %s\n"
                     "family:     %u\n"
                     "model:      %u\n"
                     "cpus:       %lu started, %u running tasks\n"
                     "protection: %s\n"
                     "nmis:       %lu\n",
                     vendor, brand, family, model, (uint64_t)smp_get_cpu_count(), sched_running_count(),
                     cpu_protection_summary(), nmis);
}

// /proc/mounts: the mount table (mount_describe)
static size_t gen_mounts(char *buf, size_t size) {
    return mount_describe(buf, size);
}

// /proc/interrupts: interrupts delivered per line or MSI vector and CPU
// (interrupts_describe)
static size_t gen_interrupts(char *buf, size_t size) {
    return interrupts_describe(buf, size);
}

// /proc/tcp: each TCP connection with its congestion window (tcp_describe)
static size_t gen_tcp(char *buf, size_t size) {
    return tcp_describe(buf, size);
}

// One read function and one ops table per file: read_<name> serves
// gen_<name>'s text, ops_<name> is what the node is created with
#define PROC_FILE(name) \
    static int64_t read_##name(vnode_t *node, size_t offset, void *buf, size_t count) { \
        (void)node; \
        return serve_text(gen_##name, offset, buf, count); \
    } \
    static const vnode_ops_t ops_##name = { .read = read_##name, .write = NULL }

PROC_FILE(version);
PROC_FILE(uptime);
PROC_FILE(timers);
PROC_FILE(meminfo);
PROC_FILE(tasks);
PROC_FILE(cpuinfo);
PROC_FILE(mounts);
PROC_FILE(interrupts);
PROC_FILE(tcp);

// --- /proc/<pid> ----------------------------------------------------------------

static const char *const state_words[] = { "ready", "running", "blocked", "zombie", "stopped" };

static vnode_t *self_link = NULL;         // /proc/self
static char self_target[12] = "0";        // Its target: the pid of whoever looked last (under the tree lock)
static bool refreshing = false;           // A refresh is under way (it looks inside /proc itself)
static bool tearing_down = false;         // procfs is removing a directory of its own (the remove ops allow it)

static const vnode_ops_t pid_dir_ops;

// The pid a /proc/<pid> node (the directory or a file in it) describes
static uint32_t pid_of(const vnode_t *node) { return (uint32_t)node->private_data; }

// Copy the slice [offset, offset + count) of text[0..len) into `buf`.
// Returns the bytes copied (0 past the end).
static int64_t serve_slice(const char *text, size_t len, size_t offset, void *buf, size_t count) {
    if (offset >= len) return 0;
    size_t n = len - offset < count ? len - offset : count;
    memcpy(buf, text + offset, n);
    return (int64_t)n;
}

// /proc/<pid>/status: who the task is and what it is doing, "Key:\tvalue"
// a line. Memory as the task records it: the heap (brk), mmap'd regions,
// and how much of those has not been touched yet (no frames behind it).
// "Syscall" is the number of the system call the task is in, or "-".
// -ENOENT once the task is gone.
static int64_t read_status(vnode_t *node, size_t offset, void *buf, size_t count) {
    char *text = kmalloc(PROC_TEXT_MAX);
    if (!text) return -ENOMEM;
    size_t len = 0;
    bool found = false;

    // Formatting takes no other lock and cannot sleep (see gen_tasks)
    uint64_t flags = sched_lock_irqsave();
    task_t *t = sched_find_task_locked(pid_of(node));
    if (t) {
        found = true;
        uint64_t mapped = 0;
        for (int i = 0; i < TASK_MAX_REGIONS; i++) {
            if (t->regions[i].end && !t->regions[i].program) mapped += t->regions[i].end - t->regions[i].start;
        }
        uint64_t heap = t->is_user ? t->brk_end - t->brk_start : 0;
        char syscall_text[12] = "-";
        if (t->in_syscall) ksnprintf(syscall_text, sizeof(syscall_text), "%u", t->in_syscall - 1);
        len = ksnprintf(text, PROC_TEXT_MAX,
                        "Name:\t%s\nState:\t%s\nPid:\t%u\nPPid:\t%u\nPgid:\t%u\n"
                        "Uid:\t%u\nRuid:\t%u\nGid:\t%u\nRgid:\t%u\nKind:\t%s\nPriority:\t%u\n"
                        "Ticks:\t%lu\nCpu:\t%u\nHeap:\t%lu kB\nMapped:\t%lu kB\nUntouched:\t%lu kB\nSyscall:\t%s\n",
                        t->name, (unsigned)t->state < 5 ? state_words[t->state] : "?", t->pid, t->parent_pid, t->pgid,
                        t->uid, t->ruid, t->gid, t->rgid, t->is_user ? "user" : "kernel", (unsigned)t->priority,
                        t->cpu_ticks, t->last_cpu, heap / 1024, mapped / 1024, (uint64_t)t->promised_pages * 4, syscall_text);
    }
    sched_unlock_irqrestore(flags);

    int64_t result = found ? serve_slice(text, len, offset, buf, count) : -ENOENT;
    kfree(text);
    return result;
}

// /proc/<pid>/cmdline: the program's arguments, each followed by a NUL, as
// execve was given them (cut to TASK_CMDLINE_MAX bytes); empty for a
// kernel task. -ENOENT once the task is gone.
static int64_t read_cmdline(vnode_t *node, size_t offset, void *buf, size_t count) {
    char *text = kmalloc(TASK_CMDLINE_MAX);
    if (!text) return -ENOMEM;
    size_t len = 0;
    bool found = false;
    uint64_t flags = sched_lock_irqsave();
    task_t *t = sched_find_task_locked(pid_of(node));
    if (t) {
        found = true;
        len = t->cmdline_len;
        memcpy(text, t->cmdline, len);
    }
    sched_unlock_irqrestore(flags);
    int64_t result = found ? serve_slice(text, len, offset, buf, count) : -ENOENT;
    kfree(text);
    return result;
}

// One line of /proc/<pid>/maps, as copied from the task
typedef struct {
    uint64_t start, end;
    int prot;
    const char *kind;          // "program", "heap", "stack", "anonymous", "file"
    vnode_t *node;             // "file": the file (alive while the tree lock is held: see read_maps)
    uint64_t offset;           // "file": where in it `start` is
} map_line_t;

#define MAPS_MAX_LINES (TASK_MAX_REGIONS + ELF_REPORTED_SEGMENTS + 2)
#define MAPS_TEXT_MAX  16384   // Every line with a file name of VFS_PATH_MAX fits

// Copy the address-space layout of task `t` into `lines` (room for
// MAPS_MAX_LINES) and its name into `name`; returns how many lines.
// Caller holds the scheduler lock.
static size_t copy_maps(const task_t *t, map_line_t *lines, char name[32]) {
    size_t n = 0;
    if (!t->is_user || t->state == TASK_ZOMBIE) return 0;  // A zombie has given its memory back
    memcpy(name, t->name, 31);
    name[31] = '\0';
    for (uint32_t i = 0; i < t->image_segment_count && i < ELF_REPORTED_SEGMENTS; i++) {
        const elf_segment_t *seg = &t->image_segments[i];
        lines[n++] = (map_line_t){ seg->start, seg->end, seg->prot, "program", NULL, 0 };
    }
    if (t->brk_end > t->brk_start) {
        lines[n++] = (map_line_t){ t->brk_start, t->brk_end, PROT_READ | PROT_WRITE, "heap", NULL, 0 };
    }
    if (t->stack_top >= USER_STACK_MAX) {
        lines[n++] = (map_line_t){ t->stack_top - USER_STACK_MAX, t->stack_top, PROT_READ | PROT_WRITE, "stack", NULL, 0 };
    }
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        const mem_region_t *r = &t->regions[i];
        if (r->end == 0 || r->program) continue;    // (Shown above, as part of the program)
        lines[n++] = (map_line_t){ r->start, r->end, r->prot, r->node ? "file" : "anonymous", r->node, r->file_offset };
    }
    return n;
}

// /proc/<pid>/maps: one line per piece of the address space, lowest first:
//   start-end perms kind [name]
// with the program's segments (named after it), the heap (brk), the stack
// (the whole 8 MiB it may grow into), and each mmap region: "anonymous",
// or "file" with the file's path and "+offset" (hex). Shared memory
// (MAP_SHARED, shmat) is not recorded by the task and is not shown.
// -ENOENT once the task is gone.
//
// The regions are the task's to change, without the scheduler lock, and a
// file region holds a pointer to the file's node. The tree lock is held
// across the copy and the formatting: a region's file is unpinned (under
// the tree lock) only after its entry is cleared (region.c), and a pinned
// node cannot be removed, so every node seen here is alive until the end.
static int64_t read_maps(vnode_t *node, size_t offset, void *buf, size_t count) {
    map_line_t *lines = kmalloc(MAPS_MAX_LINES * sizeof(map_line_t));
    char *text = kmalloc(MAPS_TEXT_MAX);
    char *path = kmalloc(VFS_PATH_MAX);
    if (!lines || !text || !path) {
        kfree(lines);
        kfree(text);
        kfree(path);
        return -ENOMEM;
    }
    char name[32] = "";
    size_t n = 0, len = 0;

    vfs_lock();
    uint64_t flags = sched_lock_irqsave();
    task_t *t = sched_find_task_locked(pid_of(node));
    bool found = t != NULL;
    if (t) n = copy_maps(t, lines, name);
    sched_unlock_irqrestore(flags);

    // Lowest first (insertion sort: a few dozen lines at most)
    for (size_t i = 1; i < n; i++) {
        map_line_t line = lines[i];
        size_t j = i;
        while (j > 0 && lines[j - 1].start > line.start) {
            lines[j] = lines[j - 1];
            j--;
        }
        lines[j] = line;
    }
    for (size_t i = 0; i < n && len < MAPS_TEXT_MAX - 1; i++) {
        const map_line_t *l = &lines[i];
        len += ksnprintf(text + len, MAPS_TEXT_MAX - len, "%012lx-%012lx %c%c%c %s",
                         l->start, l->end, (l->prot & PROT_READ) ? 'r' : '-', (l->prot & PROT_WRITE) ? 'w' : '-',
                         (l->prot & PROT_EXEC) ? 'x' : '-', l->kind);
        if (len >= MAPS_TEXT_MAX - 1) break;
        if (l->node) {
            if (vfs_path_of(l->node, path, VFS_PATH_MAX) == 0) memcpy(path, "?", 2);
            len += ksnprintf(text + len, MAPS_TEXT_MAX - len, " %s +%lx\n", path, l->offset);
        } else if (strcmp(l->kind, "program") == 0) {
            len += ksnprintf(text + len, MAPS_TEXT_MAX - len, " %s\n", name);
        } else {
            len += ksnprintf(text + len, MAPS_TEXT_MAX - len, "\n");
        }
    }
    vfs_unlock();
    if (len > MAPS_TEXT_MAX - 1) len = MAPS_TEXT_MAX - 1;

    int64_t result = found ? serve_slice(text, len, offset, buf, count) : -ENOENT;
    kfree(lines);
    kfree(text);
    kfree(path);
    return result;
}

// The descriptor number a /proc/<pid>/fd/N node stands for
static int fd_of(const vnode_t *node) { return (int)(node->private_data >> 32); }

// Describe an open file in `text` (`size` bytes, NUL-terminated), one
// line: a path for a file or directory, else what kind of thing it is.
// Returns the length. May sleep (takes the tree lock).
static size_t describe_file(const file_t *file, char *text, size_t size) {
    switch (file->kind) {
        case FILE_CONSOLE: return ksnprintf(text, size, "console\n");
        case FILE_PIPE:    return ksnprintf(text, size, file->readable ? "pipe (read end)\n" : "pipe (write end)\n");
        case FILE_SOCKET:  return ksnprintf(text, size, "socket (UDP)\n");
        case FILE_TCP:     return ksnprintf(text, size, "socket (TCP)\n");
        case FILE_VNODE:   break;
    }
    // An open node cannot be removed (its open count pins it)
    size_t len = vfs_path_of(file->node, text, size - 1);
    if (len == 0) len = ksnprintf(text, size - 1, "?");
    text[len++] = '\n';
    text[len] = '\0';
    return len;
}

// /proc/<pid>/fd/N: what descriptor N of the task refers to, one line.
// -ENOENT once the task is gone or the descriptor closed. The task can
// close it at any moment; the file is taken with a reference of our own
// under the slot lock (fd_slot_dup), so it cannot be freed while it is
// described, and that reference is dropped after (the last one, if the
// task closed it meanwhile: then the file is closed here).
static int64_t read_fd(vnode_t *node, size_t offset, void *buf, size_t count) {
    uint64_t flags = sched_lock_irqsave();
    task_t *t = sched_find_task_locked(pid_of(node));
    file_t *file = t ? fd_slot_dup(t->fds, fd_of(node)) : NULL;
    sched_unlock_irqrestore(flags);
    if (!file) return -ENOENT;

    char text[VFS_PATH_MAX + 2];
    size_t len = describe_file(file, text, sizeof(text));
    file_close(file);
    return serve_slice(text, len, offset, buf, count);
}

static const vnode_ops_t ops_status = { .read = read_status };
static const vnode_ops_t ops_cmdline = { .read = read_cmdline };
static const vnode_ops_t ops_maps = { .read = read_maps };
static const vnode_ops_t ops_fd = { .read = read_fd };

// vnode_ops_t.remove for /proc and its pid directories: nothing there is
// anyone's to remove (EPERM, root included), except procfs's own clearing
// away of a task that is gone
static int refuse_remove(vnode_t *dir, vnode_t *child) {
    (void)dir;
    (void)child;
    return tearing_down ? 0 : -EPERM;
}

// One task as a refresh sees it
typedef struct {
    uint32_t pid, uid, gid;
} proc_entry_t;

// The /proc/<pid> directory for `pid` under `dir`, or NULL
static vnode_t *pid_dir(vnode_t *dir, uint32_t pid) {
    for (vnode_t *c = dir->children; c; c = c->next) {
        if (c->ops == &pid_dir_ops && pid_of(c) == pid) return c;
    }
    return NULL;
}

// Attach a node `name` of `type` to the directory `d` (in /proc/<pid>),
// with `ops`, permission bits `mode` and d's owner; private_data is d's
// pid plus `extra` (the descriptor number, for fd/N). Returns it, or NULL.
static vnode_t *add_pid_node(vnode_t *d, const char *name, uint32_t type, const vnode_ops_t *ops,
                             uint16_t mode, uint64_t extra) {
    vnode_t *f = vfs_attach(d, name, type);
    if (!f) return NULL;
    f->ops = ops;
    f->private_data = (uint32_t)d->private_data | extra;
    f->mode = mode;
    f->uid = d->uid;
    f->gid = d->gid;
    return f;
}

// Attach a file `name` to the pid directory `d`, read with `ops`, readable
// by everyone
static void add_pid_file(vnode_t *d, const char *name, const vnode_ops_t *ops) {
    add_pid_node(d, name, VFS_FILE, ops, 0444, 0);
}

// Remove `n` from `parent`, with everything under it. Caller holds the tree
// lock and has set tearing_down (the remove ops refuse otherwise), during a
// refresh (so that looking inside fd/ does not refresh it meanwhile).
// false if something could not go.
static bool remove_pid_node(vnode_t *parent, vnode_t *n) {
    while (n->children) {
        if (!remove_pid_node(n, n->children)) return false;
    }
    return vfs_unlink(parent, n->name) == 0;
}

// vnode_ops_t.refresh for /proc/<pid>/fd: a file for each open descriptor
// of the task, named by its number (none once the task is gone). Those of
// descriptors closed since go, unless open themselves (then they read as
// ENOENT and go at a later refresh). Caller holds the tree lock.
static void fd_dir_refresh(vnode_t *dir) {
    if (refreshing) return;                    // Our own changes inside it, or /proc's
    refreshing = true;

    uint32_t mask = 0, uid = dir->uid, gid = dir->gid;
    uint64_t flags = sched_lock_irqsave();
    task_t *t = sched_find_task_locked(pid_of(dir));
    if (t) {
        mask = fd_slot_mask(t->fds);
        uid = t->uid;
        gid = t->gid;
    }
    sched_unlock_irqrestore(flags);

    dir->uid = uid;
    dir->gid = gid;
    tearing_down = true;
    for (vnode_t *c = dir->children; c;) {
        vnode_t *next = c->next;
        if (!(mask & (1u << fd_of(c))) && !c->open_count) {
            vfs_unlink(dir, c->name);
        } else {
            c->uid = uid;
            c->gid = gid;
        }
        c = next;
    }
    tearing_down = false;
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        if (!(mask & (1u << fd))) continue;
        bool present = false;
        for (vnode_t *c = dir->children; c && !present; c = c->next) present = fd_of(c) == fd;
        if (present) continue;
        char name[4];
        ksnprintf(name, sizeof(name), "%d", fd);
        add_pid_node(dir, name, VFS_FILE, &ops_fd, 0400, (uint64_t)fd << 32);
    }
    refreshing = false;
}

static const vnode_ops_t fd_dir_ops;

// Make /proc/<pid> for a task, with its files. Caller holds the tree lock.
static void add_pid_dir(vnode_t *dir, const proc_entry_t *e) {
    char name[12];
    ksnprintf(name, sizeof(name), "%u", e->pid);
    vnode_t *d = vfs_attach(dir, name, VFS_DIRECTORY);
    if (!d) return;
    d->ops = &pid_dir_ops;
    d->private_data = e->pid;
    d->mode = 0555;
    d->uid = e->uid;
    d->gid = e->gid;
    add_pid_file(d, "status", &ops_status);
    add_pid_file(d, "cmdline", &ops_cmdline);
    add_pid_node(d, "maps", VFS_FILE, &ops_maps, 0400, 0);      // They name files: the owner's only
    add_pid_node(d, "fd", VFS_DIRECTORY, &fd_dir_ops, 0500, 0);
}

// Whether `n` or anything under it is open (or someone's working directory)
static bool pid_node_in_use(const vnode_t *n) {
    if (n->open_count) return true;
    for (const vnode_t *c = n->children; c; c = c->next) if (pid_node_in_use(c)) return true;
    return false;
}

// Remove the directory `d` of a task that is gone, and everything in it,
// unless any of it is open (or someone's working directory): then it
// stays, reading as ENOENT, and the next refresh tries again. Caller holds
// the tree lock, during a refresh of /proc.
static void remove_pid_dir(vnode_t *dir, vnode_t *d) {
    if (pid_node_in_use(d)) return;
    tearing_down = true;
    remove_pid_node(dir, d);
    tearing_down = false;
}

// vnode_ops_t.refresh for /proc: point /proc/self at the reader, and make
// the pid directories match the task list (see the comment at the top).
// The task list is copied under the scheduler lock, and the tree is
// changed only after it is released. Caller holds the tree lock.
static void proc_refresh(vnode_t *dir) {
    if (refreshing) return;                    // Our own lookups inside /proc
    refreshing = true;

    task_t *me = sched_get_current_task();
    ksnprintf(self_target, sizeof(self_target), "%u", me ? me->pid : 0);
    if (self_link) self_link->size = strlen(self_target);

    // How many tasks, then a copy of them (room for a few more that start meanwhile)
    size_t count = 0;
    uint64_t flags = sched_lock_irqsave();
    for (task_t *t = sched_first_task(); t; t = t->next) count++;
    sched_unlock_irqrestore(flags);
    size_t room = count + 16;
    proc_entry_t *tasks = kmalloc(room * sizeof(proc_entry_t));
    if (!tasks) {
        refreshing = false;
        return;
    }
    size_t n = 0;
    flags = sched_lock_irqsave();
    for (task_t *t = sched_first_task(); t && n < room; t = t->next) {
        tasks[n].pid = t->pid;
        tasks[n].uid = t->uid;
        tasks[n++].gid = t->gid;
    }
    sched_unlock_irqrestore(flags);

    // Directories of tasks that are gone
    for (vnode_t *c = dir->children; c;) {
        vnode_t *next = c->next;
        if (c->ops == &pid_dir_ops) {
            bool alive = false;
            for (size_t i = 0; i < n && !alive; i++) alive = tasks[i].pid == pid_of(c);
            if (!alive) remove_pid_dir(dir, c);
        }
        c = next;
    }
    // ... and of new ones; an owner that changed (setuid) is brought up to date
    for (size_t i = 0; i < n; i++) {
        vnode_t *d = pid_dir(dir, tasks[i].pid);
        if (!d) {
            add_pid_dir(dir, &tasks[i]);
            continue;
        }
        d->uid = tasks[i].uid;
        d->gid = tasks[i].gid;
        for (vnode_t *f = d->children; f; f = f->next) {
            f->uid = tasks[i].uid;
            f->gid = tasks[i].gid;
        }
    }
    kfree(tasks);
    refreshing = false;
}

static const vnode_ops_t proc_dir_ops = { .refresh = proc_refresh, .remove = refuse_remove };
static const vnode_ops_t pid_dir_ops = { .remove = refuse_remove };
static const vnode_ops_t fd_dir_ops = { .refresh = fd_dir_refresh, .remove = refuse_remove };

// /proc/kmsg: the kernel message log. Too large for serve_text's buffer, and
// already stored as text, so it is read straight from the log.
static int64_t read_kmsg(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node;
    return (int64_t)klog_read(offset, buf, count);
}
static const vnode_ops_t ops_kmsg = { .read = read_kmsg, .write = NULL };

void procfs_init(void) {
    vfs_init();
    vnode_t *proc_dir = NULL;
    vfs_mount_dir(vfs_get_root(), "proc", &proc_dir);
    if (!proc_dir) return;

    // Each call is a no-op if the node already exists
    vfs_create_device(proc_dir, "version", &ops_version);
    vfs_create_device(proc_dir, "uptime", &ops_uptime);
    vfs_create_device(proc_dir, "timers", &ops_timers);
    vfs_create_device(proc_dir, "meminfo", &ops_meminfo);
    vfs_create_device(proc_dir, "tasks", &ops_tasks);
    vfs_create_device(proc_dir, "cpuinfo", &ops_cpuinfo);
    vfs_create_device(proc_dir, "kmsg", &ops_kmsg);
    vfs_create_device(proc_dir, "mounts", &ops_mounts);
    vfs_create_device(proc_dir, "interrupts", &ops_interrupts);
    vfs_create_device(proc_dir, "tcp", &ops_tcp);

    // /proc/self, a link whose target is rewritten at each refresh, and the
    // per-task directories that refresh keeps in step (from now on)
    if (!self_link) {
        self_link = vfs_attach(proc_dir, "self", VFS_SYMLINK);
        if (self_link) {
            self_link->data = self_target;     // Not owned: never freed
            self_link->size = strlen(self_target);
            self_link->mode = 0777;
        }
    }
    proc_dir->ops = &proc_dir_ops;
    mount_register("/proc", "proc", "kernel");
    kprintf("ProcFS Init: /proc/version, uptime, timers, meminfo, tasks, cpuinfo, kmsg, mounts, interrupts, tcp, self, <pid>/\n");
}
