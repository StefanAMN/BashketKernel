#include "test.h"
#include "boot_loader.h"

TEST(boot_loader_cpuid_test) {
    bool has_long_mode = boot_loader_check_long_mode();
    EXPECT_TRUE(has_long_mode == true);
}

void test_suite_boot(void) {
    kprintf("Suite: Phase 14 Custom x86_64 Bare-Metal Bootloader\n");
    RUN_TEST(boot_loader_cpuid_test);
}
