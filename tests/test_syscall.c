#include "test.h"
#include "syscall.h"
#include "process.h"
#include "sched.h"
#include "pmm.h"
#include "interrupts.h"

// The dispatcher called from a kernel thread: no user memory is mapped, so
// every pointer argument must be refused rather than dereferenced.
TEST(syscall_rejects_kernel_pointers) {
    struct registers regs = {0};

    struct utsname info;
    regs.rax = SYS_UNAME;
    regs.rdi = (uint64_t)&info;     // A kernel address
    EXPECT_EQ(syscall_handler(&regs), -EFAULT);

    int fds[2];
    regs.rax = SYS_PIPE;
    regs.rdi = (uint64_t)fds;
    EXPECT_EQ(syscall_handler(&regs), -EFAULT);

    regs.rax = SYS_OPEN;
    regs.rdi = (uint64_t)"/etc/motd";
    regs.rsi = 0;
    EXPECT_EQ(syscall_handler(&regs), -EFAULT);

    regs.rax = 9999;                // Unknown number
    EXPECT_EQ(syscall_handler(&regs), -ENOSYS);

    // A kernel thread has no open files at all
    static const char msg[] = "this must not be printed\n";
    regs.rax = SYS_WRITE;
    regs.rdi = 1;
    regs.rsi = (uint64_t)msg;
    regs.rdx = sizeof(msg) - 1;
    EXPECT_EQ(syscall_handler(&regs), -EBADF);
    regs.rdi = (uint64_t)-1;
    EXPECT_EQ(syscall_handler(&regs), -EBADF);
    regs.rdi = TASK_MAX_FDS;
    EXPECT_EQ(syscall_handler(&regs), -EBADF);
}

// The real test of the syscall path is a program in ring 3 making system
// calls. /bin/syscheck (user/syscheck.c) checks pointer validation, brk,
// mmap, fork, exec, wait and kill, and exits 0 only if all of it worked.
TEST(syscalls_from_ring3) {
    sched_reap();
    size_t free_before = pmm_get_free_frames();

    pid_t pid = process_spawn("/bin/syscheck", NULL);
    EXPECT_TRUE(pid > 0);

    int status = -1;
    EXPECT_EQ(process_wait(pid, &status), pid);
    EXPECT_EQ(status, 0);

    // Every process it created is gone and every frame is back
    sched_reap();
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

void test_suite_syscall(void) {
    kprintf("Suite: System Calls\n");
    RUN_TEST(syscall_rejects_kernel_pointers);
    RUN_TEST(syscalls_from_ring3);
}
