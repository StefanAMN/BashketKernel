#include "test.h"
#include "syscall.h"
#include "interrupts.h"

TEST(syscall_handler_test) {
    struct registers regs;
    regs.rax = SYS_WRITE;
    regs.rdi = 1; // stdout
    const char *msg = "Syscall Test Message\n";
    regs.rsi = (uint64_t)msg;
    regs.rdx = 21;

    int64_t ret = syscall_handler(&regs);
    EXPECT_EQ(ret, 21);

    regs.rax = 9999; // Invalid syscall
    ret = syscall_handler(&regs);
    EXPECT_EQ(ret, -1);
}

void test_suite_syscall(void) {
    kprintf("Suite: Phase 5 Syscall Subsystem\n");
    RUN_TEST(syscall_handler_test);
}
