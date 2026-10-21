#include "test.h"
#include "sched.h"
#include "pit.h"
#include "tcp.h"
#include "kprintf.h"
#include "file.h"
#include "kstring.h"

// The counts every EXPECT_ adds to (see test.h)
int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

void test_settle(void) {
    if (tcp_sockets_in_use() == 0) return;
    uint64_t deadline = timer_ticks() + 500;
    while (tcp_sockets_in_use() != 0 && timer_ticks() < deadline) sched_sleep_ticks(1);
}

bool test_file_is(const char *path, const char *text) {
    char buf[128] = {0};
    file_t *file = NULL;
    if (file_open(path, O_RDONLY, &file) != 0) return false;
    int64_t n = file_read(file, buf, sizeof(buf) - 1);
    file_close(file);
    return n == (int64_t)strlen(text) && memcmp(buf, text, (size_t)n) == 0;
}

int test_remove(const char *dir, const char *name) {
    return vfs_unlink(vfs_find(NULL, dir), name);
}

bool run_all_tests(void) {
    kprintf("\n--- Starting Kernel Unit Tests ---\n");
    
    test_suite_pmm();
    test_suite_vmm();
    test_suite_heap();
    test_suite_sched();
    test_suite_syscall();
    test_suite_elf();
    test_suite_vfs();
    test_suite_shell();
    test_suite_slab();
    test_suite_mmap();
    test_suite_apic();
    test_suite_smp();
    test_suite_virtio();
    test_suite_mouse();
    test_suite_swap();
    test_suite_pcache();
    test_suite_storage();
    test_suite_fat32();
    test_suite_process();
    test_suite_ext2();
    test_suite_net();
    test_suite_tcp();
    test_suite_rtc();
    test_suite_pipe();
    test_suite_ansi();
    test_suite_priority();
    test_suite_sem();
    test_suite_procfs();
    test_suite_tty();
    test_suite_fpu();
    test_suite_auth();
    test_suite_klog();
    test_suite_hrtimer();
    test_suite_mount();
    test_suite_shm();
    test_suite_crypto();
    test_suite_dev();
    test_suite_bench();
    test_suite_lib();
    test_suite_poll();
    
    kprintf("\n--- Test Summary ---\n");
    kprintf("Tests run: %d\n", tests_run);
    kprintf("Passed: %d\n", tests_passed);
    kprintf("Failed: %d\n", tests_failed);
    kprintf("--------------------------\n\n");
    return tests_failed == 0;
}
