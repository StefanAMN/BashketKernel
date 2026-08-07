#include "test.h"
#include "sched.h"
#include "pit.h"
#include "tcp.h"
#include "kprintf.h"

int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

// Declarations for test suites
extern void test_suite_example(void);
extern void test_suite_pmm(void);
extern void test_suite_vmm(void);
extern void test_suite_heap(void);
extern void test_suite_sched(void);
extern void test_suite_syscall(void);
extern void test_suite_elf(void);
extern void test_suite_vfs(void);
extern void test_suite_shell(void);
extern void test_suite_slab(void);
extern void test_suite_mmap(void);
extern void test_suite_apic(void);
extern void test_suite_smp(void);
extern void test_suite_storage(void);
extern void test_suite_fat32(void);
extern void test_suite_process(void);
extern void test_suite_ext2(void);
extern void test_suite_net(void);
extern void test_suite_tcp(void);
extern void test_suite_rtc(void);
extern void test_suite_pipe(void);
extern void test_suite_ansi(void);
extern void test_suite_priority(void);
extern void test_suite_sem(void);
extern void test_suite_procfs(void);
extern void test_suite_tty(void);
extern void test_suite_fpu(void);
extern void test_suite_auth(void);
extern void test_suite_klog(void);
extern void test_suite_hrtimer(void);
extern void test_suite_mount(void);
extern void test_suite_shm(void);
extern void test_suite_crypto(void);
extern void test_suite_dev(void);
extern void test_suite_bench(void);
extern void test_suite_lib(void);

void test_settle(void) {
    if (tcp_sockets_in_use() == 0) return;
    uint64_t deadline = timer_ticks() + 500;
    while (tcp_sockets_in_use() != 0 && timer_ticks() < deadline) sched_sleep_ticks(1);
}

bool run_all_tests(void) {
    kprintf("\n--- Starting Kernel Unit Tests ---\n");
    
    test_suite_example();
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
    
    kprintf("\n--- Test Summary ---\n");
    kprintf("Tests run: %d\n", tests_run);
    kprintf("Passed: %d\n", tests_passed);
    kprintf("Failed: %d\n", tests_failed);
    kprintf("--------------------------\n\n");
    return tests_failed == 0;
}
