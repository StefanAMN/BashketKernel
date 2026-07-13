#include "test.h"
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

void run_all_tests(void) {
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
    
    kprintf("\n--- Test Summary ---\n");
    kprintf("Tests run: %d\n", tests_run);
    kprintf("Passed: %d\n", tests_passed);
    kprintf("Failed: %d\n", tests_failed);
    kprintf("--------------------------\n\n");
}
