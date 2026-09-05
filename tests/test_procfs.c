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
    EXPECT_TRUE(slurp(path, text, sizeof(text)) > 0);
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

void test_suite_procfs(void) {
    kprintf("Suite: /proc\n");
    RUN_TEST(procfs_files_report_live_state);
    RUN_TEST(proc_pid_directories_follow_processes);
}
