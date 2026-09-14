/**
 * @file procfs.c
 * @brief /proc: kernel state presented as text files
 *
 * Nothing under /proc is stored. Each file has a generator function that
 * writes the current state as text; a read runs the generator and returns
 * the requested slice of what it produced.
 *
 * /proc/<pid>/ has a directory per task, with `status` and `cmdline`. They
 * are kept in step with the task list by the /proc directory's refresh
 * operation, which the VFS runs every time anything looks inside /proc: it
 * adds directories for new tasks and removes those of tasks that are gone
 * (unless someone still has one open, in which case it tries again next
 * time). The nodes hold only a pid; every read finds the task by that pid
 * under the scheduler lock, so a node never points at a freed task. A task
 * that has been reaped reads as ENOENT. /proc/self is a symbolic link
 * whose target is rewritten with the reader's pid at each refresh.
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

// /proc/meminfo: frames, promised pages, swap and the kernel heap
static size_t gen_meminfo(char *buf, size_t size) {
    return ksnprintf(buf, size,
                     "MemTotal:   %lu kB\n"
                     "MemFree:    %lu kB\n"
                     "Promised:   %lu kB\n"
                     "SwapTotal:  %lu kB\n"
                     "SwapFree:   %lu kB\n"
                     "Swapped:    %lu pages out, %lu back in\n"
                     "KernelHeap: %lu bytes in use, %lu kB mapped\n"
                     "Refused:    %lu frame requests, %lu kernel heap requests\n",
                     (uint64_t)pmm_get_total_frames() * 4,
                     (uint64_t)pmm_get_free_frames() * 4,
                     (uint64_t)region_promised_pages() * 4,
                     (uint64_t)swap_total_pages() * 4,
                     (uint64_t)swap_free_pages() * 4,
                     swap_pages_out(), swap_pages_in(),
                     (uint64_t)kheap_get_allocated_bytes(),
                     (uint64_t)kheap_get_mapped_bytes() / 1024,
                     (uint64_t)pmm_get_failed_allocations(),
                     (uint64_t)kheap_get_failed_allocations());
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
            if (t->regions[i].end) mapped += t->regions[i].end - t->regions[i].start;
        }
        uint64_t heap = t->is_user ? t->brk_end - t->brk_start : 0;
        len = ksnprintf(text, PROC_TEXT_MAX,
                        "Name:\t%s\nState:\t%s\nPid:\t%u\nPPid:\t%u\nPgid:\t%u\n"
                        "Uid:\t%u\nRuid:\t%u\nGid:\t%u\nRgid:\t%u\nKind:\t%s\nPriority:\t%u\n"
                        "Ticks:\t%lu\nCpu:\t%u\nHeap:\t%lu kB\nMapped:\t%lu kB\nUntouched:\t%lu kB\n",
                        t->name, (unsigned)t->state < 5 ? state_words[t->state] : "?", t->pid, t->parent_pid, t->pgid,
                        t->uid, t->ruid, t->gid, t->rgid, t->is_user ? "user" : "kernel", (unsigned)t->priority,
                        t->cpu_ticks, t->last_cpu, heap / 1024, mapped / 1024, (uint64_t)t->promised_pages * 4);
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

static const vnode_ops_t ops_status = { .read = read_status };
static const vnode_ops_t ops_cmdline = { .read = read_cmdline };

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

// Attach a file `name` to the pid directory `d`, read with `ops`
static void add_pid_file(vnode_t *d, const char *name, const vnode_ops_t *ops) {
    vnode_t *f = vfs_attach(d, name, VFS_FILE);
    if (!f) return;
    f->ops = ops;
    f->private_data = d->private_data;
    f->mode = 0444;
    f->uid = d->uid;
    f->gid = d->gid;
}

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
}

// Remove the directory `d` of a task that is gone, and its files, unless
// one of them is open (or someone's working directory): then it stays,
// reading as ENOENT, and the next refresh tries again. Caller holds the
// tree lock.
static void remove_pid_dir(vnode_t *dir, vnode_t *d) {
    if (d->open_count) return;
    for (vnode_t *f = d->children; f; f = f->next) if (f->open_count) return;
    tearing_down = true;
    while (d->children && vfs_unlink(d, d->children->name) == 0) { }
    if (!d->children) vfs_unlink(dir, d->name);
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
    kprintf("ProcFS Init: /proc/version, uptime, timers, meminfo, tasks, cpuinfo, kmsg, mounts, self, <pid>/\n");
}
