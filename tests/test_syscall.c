#include "test.h"
#include "syscall.h"
#include "interrupts.h"
#include "kstring.h"
#include "mmap.h"

TEST(syscall_write_test) {
    struct registers regs = {0};
    regs.rax = SYS_WRITE;
    regs.rdi = 1; // stdout
    const char *msg = "Syscall Write Test\n";
    regs.rsi = (uint64_t)msg;
    regs.rdx = 19;

    int64_t ret = syscall_handler(&regs);
    EXPECT_EQ(ret, 19);

    regs.rax = 9999; // Invalid syscall
    ret = syscall_handler(&regs);
    EXPECT_EQ(ret, -1);
}

TEST(syscall_open_close_test) {
    struct registers regs = {0};
    regs.rax = SYS_OPEN;
    regs.rdi = (uint64_t)"readme.txt";

    int64_t fd = syscall_handler(&regs);
    EXPECT_TRUE(fd >= 0);

    regs.rax = SYS_CLOSE;
    regs.rdi = (uint64_t)fd;
    int64_t ret = syscall_handler(&regs);
    EXPECT_EQ(ret, 0);
}

TEST(syscall_getpid_uname_test) {
    struct registers regs = {0};
    regs.rax = SYS_GETPID;
    int64_t pid = syscall_handler(&regs);
    EXPECT_TRUE(pid >= 0);

    struct utsname info = {0};
    regs.rax = SYS_UNAME;
    regs.rdi = (uint64_t)&info;
    int64_t ret = syscall_handler(&regs);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(strcmp(info.sysname, "BashketOS"), 0);
    EXPECT_EQ(strcmp(info.machine, "x86_64"), 0);
}

TEST(syscall_mmap_munmap_brk_test) {
    struct registers regs = {0};
    regs.rax = SYS_BRK;
    regs.rdi = 0;
    int64_t brk_val = syscall_handler(&regs);
    EXPECT_TRUE(brk_val != 0);

    regs.rax = SYS_MMAP;
    regs.rdi = 0;
    regs.rsi = 4096;
    regs.rdx = PROT_READ | PROT_WRITE;
    regs.r10 = MAP_PRIVATE | MAP_ANONYMOUS;
    regs.r8 = -1;
    regs.r9 = 0;

    int64_t ptr = syscall_handler(&regs);
    EXPECT_TRUE(ptr != (int64_t)MAP_FAILED && ptr != 0);

    regs.rax = SYS_MUNMAP;
    regs.rdi = (uint64_t)ptr;
    regs.rsi = 4096;
    int64_t ret = syscall_handler(&regs);
    EXPECT_EQ(ret, 0);
}

TEST(syscall_process_time_test) {
    struct registers regs = {0};
    regs.rax = SYS_TIME;
    int64_t ticks = syscall_handler(&regs);
    EXPECT_TRUE(ticks >= 0);

    char cwd[32] = {0};
    regs.rax = SYS_GETCWD;
    regs.rdi = (uint64_t)cwd;
    regs.rsi = sizeof(cwd);
    int64_t ret = syscall_handler(&regs);
    EXPECT_TRUE(ret != -1);
    EXPECT_EQ(strcmp(cwd, "/"), 0);
}

void test_suite_syscall(void) {
    kprintf("Suite: Phase 5 Syscall Subsystem\n");
    RUN_TEST(syscall_write_test);
    RUN_TEST(syscall_open_close_test);
    RUN_TEST(syscall_getpid_uname_test);
    RUN_TEST(syscall_mmap_munmap_brk_test);
    RUN_TEST(syscall_process_time_test);
}
