#include "test.h"
#include "kstring.h"
#include "vmm.h"
#include "vfs.h"
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
    // As after a real system call, the frame's RAX now holds the result, so
    // the call number has to be set again each time
    regs.rax = SYS_WRITE;
    regs.rdi = (uint64_t)-1;
    EXPECT_EQ(syscall_handler(&regs), -EBADF);
    regs.rax = SYS_WRITE;
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

// --- read() and write() keep user memory out of the file layer ---------------

static volatile int probe_reads, probe_writes, probe_bad_pointer, probe_window_open;

static void probe_note(const void *buf) {
    uint64_t flags;
    asm volatile("pushfq\n\tpop %0" : "=r"(flags));
    if (flags & (1ULL << 18)) probe_window_open++;           // RFLAGS.AC: user access allowed
    if ((uint64_t)buf < KERNEL_SPACE_START) probe_bad_pointer++;
}

static int64_t probe_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node;
    probe_reads++;
    probe_note(buf);
    if (offset >= 10000) return 0;                           // More than one buffer's worth in all
    size_t n = count < 10000 - offset ? count : 10000 - offset;
    memset(buf, 'x', n);
    return (int64_t)n;
}

static int64_t probe_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    (void)node; (void)offset;
    probe_writes++;
    probe_note(buf);
    return (int64_t)count;
}

static const vnode_ops_t probe_ops = { .read = probe_read, .write = probe_write };

// A device that looks at how it is called. Whatever a program passes to
// read() or write(), the code below the system call must get a kernel
// buffer, and must run with the user access window closed.
TEST(file_layer_never_sees_user_buffers) {
    vnode_t *dev = vfs_find(NULL, "/dev");
    EXPECT_TRUE(dev != NULL);
    vnode_t *probe = vfs_create_device(dev, "probe", &probe_ops);
    EXPECT_TRUE(probe != NULL);
    probe->mode = 0666;
    probe_reads = probe_writes = probe_bad_pointer = probe_window_open = 0;

    // wc reads it to the end; the shell's redirection writes to it
    char *reader[] = { "/bin/wc", "/dev/probe", NULL };
    char *writer[] = { "/bin/sh", "-c", "echo hello > /dev/probe", NULL };
    int status = -1;
    pid_t pid = process_spawn("/bin/wc", reader);
    EXPECT_TRUE(pid > 0);
    EXPECT_EQ(process_wait(pid, &status), pid);
    EXPECT_EQ(status, 0);
    pid = process_spawn("/bin/sh", writer);
    EXPECT_TRUE(pid > 0);
    EXPECT_EQ(process_wait(pid, &status), pid);
    EXPECT_EQ(status, 0);

    EXPECT_TRUE(probe_reads >= 3);        // 10000 bytes do not fit one page-sized buffer
    EXPECT_TRUE(probe_writes >= 1);
    EXPECT_EQ(probe_bad_pointer, 0);
    EXPECT_EQ(probe_window_open, 0);

    EXPECT_EQ(vfs_unlink(dev, "probe"), 0);
}

void test_suite_syscall(void) {
    kprintf("Suite: System Calls\n");
    RUN_TEST(syscall_rejects_kernel_pointers);
    RUN_TEST(syscalls_from_ring3);
    RUN_TEST(file_layer_never_sees_user_buffers);
}
