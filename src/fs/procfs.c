/**
 * @file procfs.c
 * @brief /proc: kernel state presented as text files
 *
 * Nothing under /proc is stored. Each file has a generator function that
 * writes the current state as text; a read runs the generator and returns
 * the requested slice of what it produced.
 */

#include "procfs.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "heap.h"
#include "pmm.h"
#include "pit.h"
#include "sched.h"
#include "spinlock.h"
#include "kerrno.h"
#include "cpu.h"
#include "klog.h"
#include "mount.h"
#include "smp.h"

#define PROC_TEXT_MAX 4096

typedef size_t (*proc_generator_t)(char *buf, size_t size);

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

static size_t gen_version(char *buf, size_t size) {
    return ksnprintf(buf, size, "BashketOS 0.1.0 x86_64\n");
}

static size_t gen_uptime(char *buf, size_t size) {
    uint64_t ticks = pit_get_ticks();
    uint64_t hundredths = (ticks % TIMER_HZ) * 100 / TIMER_HZ;
    return ksnprintf(buf, size, "%lu.%s%lu seconds (%lu ticks at %u Hz)\n",
                     ticks / TIMER_HZ, hundredths < 10 ? "0" : "", hundredths,
                     ticks, (unsigned)TIMER_HZ);
}

static size_t gen_meminfo(char *buf, size_t size) {
    return ksnprintf(buf, size,
                     "MemTotal:   %lu kB\n"
                     "MemFree:    %lu kB\n"
                     "KernelHeap: %lu bytes in use, %lu kB mapped\n"
                     "Refused:    %lu frame requests, %lu kernel heap requests\n",
                     (uint64_t)pmm_get_total_frames() * 4,
                     (uint64_t)pmm_get_free_frames() * 4,
                     (uint64_t)kheap_get_allocated_bytes(),
                     (uint64_t)kheap_get_mapped_bytes() / 1024,
                     (uint64_t)pmm_get_failed_allocations(),
                     (uint64_t)kheap_get_failed_allocations());
}

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

    return ksnprintf(buf, size,
                     "vendor_id:  %s\n"
                     "model name: %s\n"
                     "family:     %u\n"
                     "model:      %u\n"
                     "cpus:       %lu started, 1 running tasks\n"
                     "protection: %s\n",
                     vendor, brand, family, model, (uint64_t)smp_get_cpu_count(), cpu_protection_summary());
}

static size_t gen_mounts(char *buf, size_t size) {
    return mount_describe(buf, size);
}

// One read function and one ops table per file
#define PROC_FILE(name) \
    static int64_t read_##name(vnode_t *node, size_t offset, void *buf, size_t count) { \
        (void)node; \
        return serve_text(gen_##name, offset, buf, count); \
    } \
    static const vnode_ops_t ops_##name = { .read = read_##name, .write = NULL }

PROC_FILE(version);
PROC_FILE(uptime);
PROC_FILE(meminfo);
PROC_FILE(tasks);
PROC_FILE(cpuinfo);
PROC_FILE(mounts);

// /proc/kmsg: the kernel message log. Too large for serve_text's buffer, and
// already stored as text, so it is read straight from the log.
static int64_t read_kmsg(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node;
    return (int64_t)klog_read(offset, buf, count);
}
static const vnode_ops_t ops_kmsg = { .read = read_kmsg, .write = NULL };

void procfs_init(void) {
    vfs_init();
    vnode_t *proc_dir = vfs_create_dir(vfs_get_root(), "proc");
    if (!proc_dir) return;

    // Each call is a no-op if the node already exists
    vfs_create_device(proc_dir, "version", &ops_version);
    vfs_create_device(proc_dir, "uptime", &ops_uptime);
    vfs_create_device(proc_dir, "meminfo", &ops_meminfo);
    vfs_create_device(proc_dir, "tasks", &ops_tasks);
    vfs_create_device(proc_dir, "cpuinfo", &ops_cpuinfo);
    vfs_create_device(proc_dir, "kmsg", &ops_kmsg);
    vfs_create_device(proc_dir, "mounts", &ops_mounts);
    mount_register("/proc", "proc", "kernel");
    kprintf("ProcFS Init: /proc/version, uptime, meminfo, tasks, cpuinfo, kmsg, mounts\n");
}
