#include "../src/test.h"
#include "../src/kprintf.h"

int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

// Declarations for test suites
extern void test_suite_example(void);
extern void test_suite_gdt(void);
extern void test_suite_idt(void);
extern void test_suite_pic(void);

void run_all_tests(void) {
    kprintf("\n--- Starting Kernel Unit Tests ---\n");

    test_suite_example();
    test_suite_gdt();
    test_suite_idt();
    test_suite_pic();

    kprintf("\n--- Test Summary ---\n");
    kprintf("Tests run: %d\n", tests_run);
    kprintf("Passed: %d\n", tests_passed);
    kprintf("Failed: %d\n", tests_failed);
    kprintf("--------------------------\n\n");
}
