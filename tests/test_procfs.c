#include "test.h"
#include "procfs.h"
#include "vfs.h"
#include "file.h"
#include "pmm.h"
#include "heap.h"
#include "kstring.h"

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

void test_suite_procfs(void) {
    kprintf("Suite: /proc\n");
    RUN_TEST(procfs_files_report_live_state);
}
