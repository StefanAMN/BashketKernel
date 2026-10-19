#include "test.h"
#include "procfs.h"
#include "vfs.h"
#include "file.h"
#include "pmm.h"
#include "heap.h"
#include "kstring.h"
#include "kerrno.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "smp.h"
#include "timer.h"
#include "hrtimer.h"

// Read a whole /proc file through the open-file layer, in small pieces so
// the offset handling is exercised too.
static size_t slurp(const char *path, char *out, size_t size) {
    file_t *file = NULL;
    if (file_open(path, O_RDONLY, &file) != 0) return 0;

    size_t total = 0;
    int64_t n;
    while (total + 1 < size && (n = file_read(file, out + total, (size - 1 - total) < 7 ? (size - 1 - total) : 7)) > 0) {
        total += (size_t)n;
    }

    out[total] = '\0';
    file_close(file);
    return total;
}

// The same in one read, for the status of a process that is running: each
// read makes the text afresh, and slurp's small pieces could come from two
// moments of it (a State that changes length shifts every line after it;
// "Pid:" was once lost that way and the search found "PPid:" instead).
// PLAN.md lists that tearing as a limit.
static size_t slurp_live(const char *path, char *out, size_t size) {
    file_t *file = NULL;
    if (file_open(path, O_RDONLY, &file) != 0) return 0;
    int64_t n = file_read(file, out, size - 1);
    file_close(file);
    size_t total = n > 0 ? (size_t)n : 0;
    out[total] = '\0';
    return total;
}

// Value of the number that follows `label` in `text`, or -1
static int64_t number_after(const char *text, const char *label) {
    const char *p = strstr(text, label);
    if (!p) return -1;
    p += strlen(label);
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;

    int64_t value = 0;
    while (*p >= '0' && *p <= '9') value = value * 10 + (*p++ - '0');
    return value;
}

TEST(procfs_files_report_live_state) {
    vfs_find(NULL, "/proc/version");   // Brings /proc/<pid> up to date first: that is not what is counted
    size_t heap_before = kheap_get_allocated_bytes();
    char text[1024];

    EXPECT_TRUE(slurp("/proc/version", text, sizeof(text)) > 0);
    EXPECT_EQ(strncmp(text, "BashketOS", 9), 0);

    // meminfo agrees with the allocator it reports on
    EXPECT_TRUE(slurp("/proc/meminfo", text, sizeof(text)) > 0);
    EXPECT_EQ(number_after(text, "MemTotal:"), (int64_t)pmm_get_total_frames() * 4);
    EXPECT_EQ(number_after(text, "MemFree:"), (int64_t)pmm_get_free_frames() * 4);

    // ... and changes when memory is allocated
    void *frame = pmm_alloc_frame();
    EXPECT_TRUE(slurp("/proc/meminfo", text, sizeof(text)) > 0);
    EXPECT_EQ(number_after(text, "MemFree:"), (int64_t)pmm_get_free_frames() * 4);
    pmm_free_frame(frame);

    // The task list includes the task running this test
    EXPECT_TRUE(slurp("/proc/tasks", text, sizeof(text)) > 0);
    EXPECT_TRUE(strstr(text, "kernel_main") != NULL);
    EXPECT_TRUE(strstr(text, "running") != NULL);

    EXPECT_TRUE(slurp("/proc/uptime", text, sizeof(text)) > 0);
    EXPECT_TRUE(strstr(text, "ticks") != NULL);

    EXPECT_TRUE(slurp("/proc/cpuinfo", text, sizeof(text)) > 0);
    EXPECT_TRUE(strstr(text, "vendor_id:") != NULL);

    // /proc files can not be written
    file_t *file = NULL;
    EXPECT_EQ(file_open("/proc/version", O_WRONLY, &file), 0);
    EXPECT_TRUE(file_write(file, "x", 1) < 0);
    file_close(file);

    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

// "/proc/<pid>/<file>" into path[48]
static void proc_path(char path[48], pid_t pid, const char *file) {
    ksnprintf(path, 48, "/proc/%d%s%s", (int)pid, file ? "/" : "", file ? file : "");
}

// A process's directory appears with it, describes it, reads as ENOENT once
// it is reaped (for a file someone still has open), and is gone after that,
// leaving no memory behind; /proc/self is whoever looks
TEST(proc_pid_directories_follow_processes) {
    char path[48], text[1024];
    vfs_find(NULL, "/proc/version");                     // /proc up to date before counting
    size_t heap_before = kheap_get_allocated_bytes();

    char *argv[] = { "/bin/spin", "first argument", "2", NULL };
    pid_t pid = process_spawn("/bin/spin", argv);
    EXPECT_TRUE(pid > 0);
    if (pid <= 0) return;

    proc_path(path, pid, NULL);
    vnode_t *dir = vfs_find(NULL, path);
    EXPECT_TRUE(dir != NULL && dir->type == VFS_DIRECTORY);
    proc_path(path, pid, "status");
    EXPECT_TRUE(slurp_live(path, text, sizeof(text)) > 0);
    EXPECT_TRUE(strstr(text, "Name:\tspin\n") != NULL && strstr(text, "Kind:\tuser\n") != NULL);
    EXPECT_EQ(number_after(text, "Pid:\t"), (int64_t)pid);
    EXPECT_EQ(number_after(text, "Uid:\t"), 0);
    proc_path(path, pid, "cmdline");
    size_t len = slurp(path, text, sizeof(text));
    static const char expected[] = "/bin/spin\0first argument\0" "2";   // Each followed by a NUL
    EXPECT_EQ(len, sizeof(expected));
    EXPECT_EQ(memcmp(text, expected, sizeof(expected)), 0);

    // /proc/self is the reader: here the kernel task running the tests
    vnode_t *self = NULL;
    EXPECT_EQ(vfs_lookup(NULL, "/proc/self", false, &self), 0);
    char target[16];
    int64_t n = vfs_readlink(self, target, sizeof(target) - 1);
    EXPECT_TRUE(n > 0);
    target[n > 0 ? n : 0] = '\0';
    EXPECT_EQ(number_after(target, ""), (int64_t)sched_get_current_task()->pid);

    // Held open across the end of the process: reads as ENOENT, not stale
    file_t *held = NULL;
    proc_path(path, pid, "status");
    EXPECT_EQ(file_open(path, O_RDONLY, &held), 0);
    EXPECT_EQ(process_kill(pid, SIGKILL), 0);
    int status = 0;
    EXPECT_EQ(process_wait(pid, &status), pid);
    if (held) {
        EXPECT_EQ(file_read(held, text, sizeof(text)), -ENOENT);
        file_close(held);
    }
    proc_path(path, pid, NULL);
    EXPECT_TRUE(vfs_find(NULL, path) == NULL);           // Gone at the next look
    EXPECT_TRUE(vfs_find(NULL, "/proc/999999") == NULL);
    EXPECT_EQ(vfs_unlink(vfs_find(NULL, "/proc/0"), "status"), -EPERM);  // Not even the kernel's to remove
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

// /proc/<pid>/maps and fd/: a program's segments and stack, its three
// console descriptors, the directories torn down with the process (fd/
// included, though it has files of its own), and no memory left behind
TEST(proc_pid_maps_and_fds) {
    char path[48], text[1024];
    vfs_find(NULL, "/proc/version");
    size_t heap_before = kheap_get_allocated_bytes();

    pid_t pid = process_spawn("/bin/spin", NULL);
    EXPECT_TRUE(pid > 0);
    if (pid <= 0) return;
    proc_path(path, pid, "maps");
    EXPECT_TRUE(slurp(path, text, sizeof(text)) > 0);
    EXPECT_TRUE(strstr(text, " r-x program spin\n") != NULL);
    EXPECT_TRUE(strstr(text, " rw- stack\n") != NULL);
    // Lowest first: the program (at 4 GiB and up) before the stack (near the top)
    char *program = strstr(text, "program"), *stack = strstr(text, "stack");
    EXPECT_TRUE(program && stack && program < stack);

    proc_path(path, pid, "fd");
    vnode_t *fds = vfs_find(NULL, path);
    EXPECT_TRUE(fds != NULL && fds->type == VFS_DIRECTORY);
    proc_path(path, pid, "fd/2");
    EXPECT_TRUE(slurp(path, text, sizeof(text)) > 0);
    EXPECT_EQ(strcmp(text, "console\n"), 0);
    proc_path(path, pid, "fd/3");
    EXPECT_TRUE(vfs_find(NULL, path) == NULL);           // Not open

    EXPECT_EQ(process_kill(pid, SIGKILL), 0);
    int status = 0;
    EXPECT_EQ(process_wait(pid, &status), pid);
    proc_path(path, pid, NULL);
    EXPECT_TRUE(vfs_find(NULL, path) == NULL);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

// A reference taken from a slot outlives the slot being emptied and its
// file closed: what /proc's reader of another process's table relies on
TEST(fd_slot_reference_outlives_close) {
    file_t *fds[TASK_MAX_FDS] = { 0 };
    file_t *file = NULL;
    EXPECT_EQ(file_open("/proc/version", O_RDONLY, &file), 0);
    if (!file) return;
    EXPECT_EQ(fd_install(fds, file), 0);
    EXPECT_EQ(fd_slot_mask(fds), 1u);

    file_t *mine = fd_slot_dup(fds, 0);
    EXPECT_TRUE(mine == file);
    EXPECT_TRUE(fd_slot_dup(fds, 1) == NULL && fd_slot_dup(fds, -1) == NULL && fd_slot_dup(fds, TASK_MAX_FDS) == NULL);
    file_close(fd_slot_set(fds, 0, NULL));                // The owner closes it
    EXPECT_EQ(fd_slot_mask(fds), 0u);
    char text[16];
    EXPECT_TRUE(file_read(mine, text, sizeof(text)) > 0);    // Still a working file
    EXPECT_EQ(mine->refs, 1u);
    file_close(mine);
}

// "Syscall" names the system call a process is in: the number while it is
// asleep in sleep, "-" for the kernel task reading it (which is in none)
TEST(proc_pid_status_names_the_system_call) {
    char path[48], text[1024];
    char *argv[] = { "/bin/sleep", "30", NULL };
    pid_t pid = process_spawn("/bin/sleep", argv);
    EXPECT_TRUE(pid > 0);
    if (pid <= 0) return;
    task_t *task = sched_find_task((uint32_t)pid);
    EXPECT_TRUE(task != NULL);
    if (task) TEST_WAIT(task->in_syscall == SYS_SLEEP + 1, 300);

    proc_path(path, pid, "status");
    EXPECT_TRUE(slurp_live(path, text, sizeof(text)) > 0);
    EXPECT_EQ(number_after(text, "Syscall:\t"), (int64_t)SYS_SLEEP);
    EXPECT_TRUE(number_after(text, "Size:\t") > 0);              // Its program at least

    EXPECT_TRUE(slurp("/proc/self/status", text, sizeof(text)) > 0);
    EXPECT_TRUE(strstr(text, "Syscall:\t-\n") != NULL);
    EXPECT_TRUE(strstr(text, "Size:\t0 kB\n") != NULL);         // A kernel task has no address space of its own

    EXPECT_EQ(process_kill(pid, SIGKILL), 0);
    int status = 0;
    EXPECT_EQ(process_wait(pid, &status), pid);
}

// The "busy ticks" of each CPU in /proc/cpuinfo: sum and number of lines
static uint64_t busy_ticks(const char *text, size_t *cpus) {
    uint64_t sum = 0;
    *cpus = 0;
    for (const char *p = strstr(text, "\ncpu"); p; p = strstr(p + 1, "\ncpu")) {
        if (p[4] < '0' || p[4] > '9') continue;      // "cpus:", not "cpuN:"
        const char *colon = strstr(p, ":");
        if (!colon) break;
        int64_t n = number_after(colon, ":");
        if (n < 0) continue;
        sum += (uint64_t)n;
        (*cpus)++;
    }
    return sum;
}

// /proc/cpuinfo counts, for each CPU, the ticks that found it running a
// task: they grow while this task computes, and hardly at all while every
// CPU is idle (the idle task is not counted, so periodic and one-shot
// timers mean the same)
TEST(proc_cpuinfo_counts_busy_ticks_per_cpu) {
    static char text[2048];
    size_t cpus = 0;
    EXPECT_TRUE(slurp("/proc/cpuinfo", text, sizeof(text)) > 0);
    uint64_t before = busy_ticks(text, &cpus);
    EXPECT_EQ(cpus, smp_get_cpu_count());

    uint64_t until = timer_ticks() + 5;
    while (timer_ticks() < until) asm volatile("pause");
    EXPECT_TRUE(slurp("/proc/cpuinfo", text, sizeof(text)) > 0);
    uint64_t computed = busy_ticks(text, &cpus);
    EXPECT_TRUE(computed > before);

    nanosleep(300000000ULL);                       // 30 ticks with nothing to do
    EXPECT_TRUE(slurp("/proc/cpuinfo", text, sizeof(text)) > 0);
    uint64_t slept = busy_ticks(text, &cpus);
    EXPECT_TRUE(slept - computed < 15);
}

// A reader of /proc/<pid>/status walks the process's page tables (its
// Resident line). Processes that exit or exec under it, on other CPUs,
// free tables: the reader must never walk one being freed (the swap of
// the tables happens under the scheduler lock it holds) and never report
// more than there is. Each round is a shell that forks a child, which
// execs /bin/false; both exit. The reader reads the shell's pid and the
// next few (the child's among them).
static volatile pid_t watched_pid;
static volatile int reader_stop, reader_done;
static volatile uint64_t reader_reads, reader_absurd;

// Reads the status of the watched pid and the three after it until told
// to stop, counting the reads that found a process and those whose
// Resident was more than memory holds
static void status_reader_entry(void) {
    static char text[1024];
    char path[48];
    while (!reader_stop) {
        pid_t base = watched_pid;
        if (base <= 0) { sched_yield(); continue; }
        for (pid_t pid = base; pid < base + 4; pid++) {
            // In one read, as cat does: each read makes the text afresh, so
            // slurp's small pieces could come from two moments of a process
            // that is changing (and tear the line apart)
            proc_path(path, pid, "status");
            file_t *file = NULL;
            if (file_open(path, O_RDONLY, &file) != 0) continue;     // Not there (yet, or any more)
            int64_t n = file_read(file, text, sizeof(text) - 1);
            file_close(file);
            if (n <= 0) continue;
            text[n] = '\0';
            int64_t resident = number_after(text, "Resident:\t");
            if (resident < 0 || (uint64_t)resident > pmm_get_total_frames() * 4) {
                if (reader_absurd++ == 0) kprintf("    pid %d read as: %s\n", (int)pid, text);
            }
            reader_reads++;
        }
    }
    reader_done = 1;
}

TEST(status_of_exiting_and_execing_processes) {
    reader_stop = 0; reader_done = 0; reader_reads = 0; reader_absurd = 0; watched_pid = 0;
    EXPECT_TRUE(task_create("status-reader", status_reader_entry) != NULL);
    char *argv[] = { "/bin/sh", "-c", "/bin/false", NULL };
    for (int round = 0; round < 40; round++) {
        pid_t pid = process_spawn("/bin/sh", argv);
        EXPECT_TRUE(pid > 0);
        if (pid <= 0) break;
        watched_pid = pid;
        int status = 0;
        EXPECT_EQ(process_wait(pid, &status), pid);
        EXPECT_EQ(status, 1);
    }
    watched_pid = 0;
    reader_stop = 1;
    TEST_WAIT(reader_done, 300);
    EXPECT_TRUE(reader_done);
    kprintf("    %lu status reads of processes exiting and exec'ing\n", reader_reads);
    EXPECT_TRUE(reader_reads > 40);
    EXPECT_TRUE(reader_absurd == 0);
    size_t reaped = 0;
    TEST_WAIT((reaped += sched_reap()) >= 1, 300);
}

void test_suite_procfs(void) {
    kprintf("Suite: /proc\n");
    RUN_TEST(procfs_files_report_live_state);
    RUN_TEST(proc_pid_directories_follow_processes);
    RUN_TEST(proc_pid_maps_and_fds);
    RUN_TEST(fd_slot_reference_outlives_close);
    RUN_TEST(proc_pid_status_names_the_system_call);
    RUN_TEST(proc_cpuinfo_counts_busy_ticks_per_cpu);
    RUN_TEST(status_of_exiting_and_execing_processes);
}
