/* Exercises the system call interface from ring 3 and exits 0 only if every
 * check passes. The kernel test suite runs this and checks the exit status. */
#include "ulib.h"

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { printf("syscheck: FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
    } while (0)

static void check_bad_pointers(void) {
    /* The kernel must refuse pointers outside our address space ... */
    CHECK(write(1, (void *)0xffffffff80000000, 16) == -EFAULT);
    CHECK(write(1, (void *)0, 1) == -EFAULT);
    CHECK(read(0, (void *)0xffff800000000000, 1) == -EFAULT);
    CHECK(syscall1(SYS_UNAME, 0xffffffff80000000) == -EFAULT);
    CHECK(waitpid(-1, (int *)0xffffffff80000000) == -EFAULT);
    /* ... a range that starts valid and runs off the end of the stack ... */
    char local = 'x';
    CHECK(write(1, &local, 1UL << 40) == -EFAULT);
    /* ... a length that wraps around the address space ... */
    CHECK(write(1, &local, (size_t)-1) == -EFAULT);
    /* ... and writing into read-only memory on our behalf. */
    CHECK(read(0, (void *)(unsigned long)check_bad_pointers, 1) == -EFAULT);
    CHECK(execve((const char *)0xffffffff80000000, NULL) == -EFAULT);
}

static void check_basics(void) {
    CHECK(getpid() > 0);
    CHECK(write(1, "", 0) == 0);
    CHECK(write(7, "x", 1) == -EBADF);
    CHECK(syscall0(9999) == -ENOSYS);
    CHECK(uptime_ticks() >= 0);
    CHECK(execve("/bin/does-not-exist", NULL) == -ENOENT);
    CHECK(waitpid(-1, NULL) == -ECHILD);

    CHECK(set_priority(2) == 0 && set_priority(0) == 0 && set_priority(1) == 0);
    CHECK(set_priority(3) == -EINVAL && set_priority(-1) == -EINVAL);

    char cwd[8];
    CHECK(syscall2(SYS_GETCWD, cwd, sizeof(cwd)) > 0 && strcmp(cwd, "/") == 0);
}

static void check_memory(void) {
    /* brk: grow, use, shrink */
    char *base = sbrk(0);
    CHECK(sbrk(3 * 4096 + 100) == base);
    memset(base, 0xAB, 3 * 4096 + 100);
    CHECK((unsigned char)base[3 * 4096 + 99] == 0xAB);
    CHECK(sbrk(-(3 * 4096 + 100)) != (void *)-1);
    CHECK(sbrk(0) == base);
    CHECK(sbrk(1L << 40) == (void *)-1);              /* Absurd request refused */

    /* mmap gives zeroed, writable memory; munmap takes it away */
    unsigned char *map = mmap_anon(2 * 4096);
    CHECK(map != NULL);
    if (map) {
        CHECK(map[0] == 0 && map[2 * 4096 - 1] == 0);
        map[0] = 1;
        map[2 * 4096 - 1] = 2;
        CHECK(munmap(map, 2 * 4096) == 0);
        CHECK(write(1, map, 1) == -EFAULT);           /* No longer mapped */
    }
    CHECK(munmap((void *)0x400000, 4096) == -EINVAL); /* Cannot unmap our own code */

    /* malloc on top of brk */
    char *a = malloc(100), *b = malloc(5000);
    CHECK(a && b && a != b);
    if (a && b) {
        memset(a, 1, 100);
        memset(b, 2, 5000);
        CHECK(a[99] == 1 && b[0] == 2 && b[4999] == 2);
    }
    free(a);
    free(b);
}

static int global_counter = 100;

static void check_fork(void) {
    int stack_value = 7;
    long pid = fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        /* Child: has its own copy of everything */
        global_counter += 1;
        stack_value += 1;
        exit(global_counter == 101 && stack_value == 8 ? 42 : 1);
    }

    int status = -1;
    CHECK(waitpid(pid, &status) == pid);
    CHECK(status == 42);
    CHECK(global_counter == 100);   /* The child's writes did not reach us */
    CHECK(stack_value == 7);
    CHECK(waitpid(pid, &status) == -ECHILD); /* Already collected */

    /* fork + exec + wait: run another program and get its exit status */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/hello", "from", "syscheck", NULL };
        execve("/bin/hello", args);
        exit(99); /* Only reached if exec failed */
    }
    status = -1;
    CHECK(waitpid(-1, &status) == pid);
    CHECK(status == 0);

    /* A child that crashes is killed; the parent sees a signal-style status */
    pid = fork();
    if (pid == 0) {
        volatile int *null = (int *)0;
        *null = 1;
        exit(0);
    }
    status = -1;
    CHECK(waitpid(pid, &status) == pid);
    CHECK(status == 128 + 11);

    /* kill() ends a child that would otherwise run forever */
    pid = fork();
    if (pid == 0) {
        for (;;) yield();
    }
    CHECK(kill(pid, 9) == 0);
    CHECK(kill(pid, 9) == 0);                 /* Asking twice is harmless */
    status = -1;
    CHECK(waitpid(pid, &status) == pid);
    CHECK(status == 128 + 9);
    CHECK(kill(pid, 9) == -ESRCH);
    CHECK(kill(pid, 0) == -ESRCH && kill(getpid(), -1) == -ESRCH); /* Bad signal numbers */

    /* A child stuck in a loop that makes no system calls can still be killed:
     * the timer interrupt is its safe point */
    pid = fork();
    if (pid == 0) {
        volatile unsigned long n = 0;
        for (;;) n++;
    }
    sleep_ms(30);
    CHECK(kill(pid, 15) == 0);
    status = -1;
    CHECK(waitpid(pid, &status) == pid);
    CHECK(status == 128 + 15);

    /* So can one that is asleep */
    pid = fork();
    if (pid == 0) {
        for (;;) sleep_ms(100000);
    }
    sleep_ms(30);
    CHECK(kill(pid, 9) == 0);
    status = -1;
    CHECK(waitpid(pid, &status) == pid);
    CHECK(status == 128 + 9);
}

static void check_files(void) {
    char buf[64];

    CHECK(open("/no/such/file", O_RDONLY) == -ENOENT);
    CHECK(open((const char *)0xffffffff80000000, O_RDONLY) == -EFAULT);
    CHECK(open("/tmp", O_WRONLY) == -EISDIR);
    CHECK(close(9) == -EBADF);
    CHECK(close(-1) == -EBADF);
    CHECK(read(9, buf, 1) == -EBADF);
    CHECK(lseek(0, 0, SEEK_SET) == -ESPIPE);          /* The console cannot seek */

    /* Create, write, reopen, read back */
    long fd = open("/tmp/syscheck.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 3);
    CHECK(write((int)fd, "hello file", 10) == 10);
    CHECK(write((int)fd, (void *)0xffffffff80000000, 4) == -EFAULT);
    CHECK(close((int)fd) == 0);
    CHECK(write((int)fd, "x", 1) == -EBADF);           /* Closed means closed */

    fd = open("/tmp/syscheck.txt", O_RDONLY);
    CHECK(fd >= 3);
    memset(buf, 0, sizeof(buf));
    CHECK(read((int)fd, buf, sizeof(buf)) == 10);
    CHECK(strcmp(buf, "hello file") == 0);
    CHECK(read((int)fd, buf, sizeof(buf)) == 0);       /* End of file */
    CHECK(lseek((int)fd, 6, SEEK_SET) == 6);
    CHECK(read((int)fd, buf, 4) == 4 && memcmp(buf, "file", 4) == 0);
    CHECK(write((int)fd, "x", 1) == -EBADF);           /* Read-only descriptor */
    CHECK(read((int)fd, (void *)(unsigned long)check_basics, 4) == -EFAULT); /* Into our code */
    CHECK(unlink("/tmp/syscheck.txt") == -EBUSY);      /* Still open */
    CHECK(close((int)fd) == 0);

    /* The program files from the initrd are readable like any other */
    fd = open("/bin/hello", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(read((int)fd, buf, 4) == 4 && memcmp(buf, "\177ELF", 4) == 0);
    close((int)fd);

    /* Directories */
    CHECK(mkdir("/tmp/syscheck.d") == 0);
    CHECK(mkdir("/tmp/syscheck.d") == -EEXIST);
    fd = open("/tmp/syscheck.d/inner", O_WRONLY | O_CREAT);
    CHECK(fd >= 0);
    close((int)fd);
    CHECK(unlink("/tmp/syscheck.d") == -ENOTEMPTY);

    fd = open("/tmp/syscheck.d", O_RDONLY);
    struct dirent entry;
    CHECK(readdir((int)fd, &entry) == 1 && strcmp(entry.name, "inner") == 0 && entry.type == DT_FILE);
    CHECK(readdir((int)fd, &entry) == 0);
    CHECK(readdir((int)fd, (struct dirent *)0xffffffff80000000) == -EFAULT);
    CHECK(read((int)fd, buf, 1) == -EISDIR);
    close((int)fd);
    CHECK(readdir(1, &entry) == -ENOTDIR);

    CHECK(unlink("/tmp/syscheck.d/inner") == 0);
    CHECK(unlink("/tmp/syscheck.d") == 0);
    CHECK(unlink("/tmp/syscheck.txt") == 0);
    CHECK(unlink("/tmp/syscheck.txt") == -ENOENT);

    /* Running out of descriptors is an error, and closing frees them again */
    int opened[32], count = 0;
    long last;
    while ((last = open("/etc/motd", O_RDONLY)) >= 0 && count < 32) opened[count++] = (int)last;
    CHECK(last == -EMFILE);
    CHECK(count > 0 && count < 32);
    while (count > 0) CHECK(close(opened[--count]) == 0);
}

static void check_pipes(void) {
    int fds[2];
    char buf[32];

    CHECK(pipe((int *)0xffffffff80000000) == -EFAULT);
    CHECK(pipe(fds) == 0);
    CHECK(fds[0] >= 3 && fds[1] >= 3 && fds[0] != fds[1]);
    CHECK(write(fds[0], "x", 1) == -EBADF);            /* Wrong end */
    CHECK(read(fds[1], buf, 1) == -EBADF);

    /* Parent to child, and the child's answer comes back as its exit status */
    long pid = fork();
    if (pid == 0) {
        close(fds[1]);                                 /* Or we would never see end-of-file */
        long total = 0, n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) total += n;
        exit(n == 0 ? (int)total : 99);
    }
    close(fds[0]);
    CHECK(write(fds[1], "twelve bytes", 12) == 12);
    sleep_ms(20);                                      /* The child is now asleep in read() */
    CHECK(write(fds[1], "+five", 5) == 5);
    close(fds[1]);                                     /* End of file for the child */
    int status = -1;
    CHECK(waitpid(pid, &status) == pid);
    CHECK(status == 17);

    /* A pipe nobody can read from any more refuses writes */
    CHECK(pipe(fds) == 0);
    close(fds[0]);
    CHECK(write(fds[1], "x", 1) == -EPIPE);
    close(fds[1]);

    /* dup2: make standard output a pipe, as a shell does for "a | b" */
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        printf("via stdout");
        exit(0);
    }
    close(fds[1]);
    memset(buf, 0, sizeof(buf));
    long got = 0, n;
    while ((n = read(fds[0], buf + got, sizeof(buf) - 1 - (size_t)got)) > 0) got += n;
    CHECK(strcmp(buf, "via stdout") == 0);
    close(fds[0]);
    waitpid(pid, &status);
    CHECK(dup2(99, 1) == -EBADF);
    CHECK(dup2(1, 99) == -EBADF);

    /* A child killed while blocked in read() releases its end of the pipe */
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        close(fds[1]);
        read(fds[0], buf, 1);                          /* Sleeps forever */
        exit(0);
    }
    close(fds[0]);
    sleep_ms(20);
    CHECK(kill(pid, 9) == 0);
    CHECK(waitpid(pid, &status) == pid && status == 128 + 9);
    CHECK(write(fds[1], "x", 1) == -EPIPE);            /* Its read end went with it */
    close(fds[1]);
}

/* Uses about `depth` * 4 KiB of stack */
__attribute__((noinline)) static unsigned long recurse(int depth) {
    volatile char page[4096];
    page[0] = (char)depth;
    page[4095] = 1;
    if (depth == 0) return 0;
    return recurse(depth - 1) + (unsigned long)page[0] + (unsigned long)page[4095] - (unsigned long)(char)depth;
}

static void check_stack_growth(void) {
    /* The stack starts at 16 KiB and grows as it is used: 2 MiB works */
    CHECK(recurse(500) == 500);

    /* A child inherits the grown stack and can grow its own further */
    long pid = fork();
    if (pid == 0) exit(recurse(1000) == 1000 ? 0 : 1);
    int status = -1;
    CHECK(waitpid(pid, &status) == pid && status == 0);

    /* Runaway recursion hits the limit (8 MiB) and is killed, not the system */
    pid = fork();
    if (pid == 0) exit((int)recurse(1 << 20));
    status = -1;
    CHECK(waitpid(pid, &status) == pid);
    CHECK(status == 128 + 11);

    /* The kernel does not grow the stack on a program's behalf: a buffer
     * below the mapped part is simply a bad address */
    char here;
    unsigned long far_below = (unsigned long)&here - (4UL << 20);
    CHECK(read(0, (void *)far_below, 1) == -EFAULT);
}

/* Copy-on-write: after fork both processes share every page until one writes. */
static void check_copy_on_write(void) {
    enum { PAGES = 64 };
    unsigned char *big = mmap_anon(PAGES * 4096);
    int *shared_status = mmap_anon(4096);
    CHECK(big != NULL && shared_status != NULL);
    if (!big || !shared_status) return;

    for (int i = 0; i < PAGES; i++) big[i * 4096] = (unsigned char)i;

    long pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* The child sees the parent's data, and its writes stay private */
        int ok = 1;
        for (int i = 0; i < PAGES; i++) {
            if (big[i * 4096] != (unsigned char)i) ok = 0;
            big[i * 4096] = 0xEE;
        }
        /* The kernel writing into a still-shared page must copy it too */
        struct { char sysname[65]; char rest[4 * 65]; } *info = (void *)(big + 4096 + 8);
        if (syscall1(SYS_UNAME, info) != 0 || strcmp(info->sysname, "BashketOS") != 0) ok = 0;
        exit(ok ? 7 : 1);
    }

    /* The parent has not touched shared_status since the fork, so the page is
     * still shared when the kernel stores the exit status through it. */
    CHECK(waitpid(pid, shared_status) == pid);
    CHECK(*shared_status == 7);

    for (int i = 0; i < PAGES; i++) {
        if (big[i * 4096] != (unsigned char)i) { CHECK(!"child write reached the parent"); break; }
    }
    CHECK(big[4096 + 8] == 0);             /* Nor did the kernel's write for the child */

    /* The parent can still write its own pages after the child is gone */
    big[0] = 0x55;
    CHECK(big[0] == 0x55);

    /* Forks of forks: a chain of processes each bumps its private copy */
    static int depth_value = 0;
    long first = fork();
    if (first == 0) {
        depth_value = 1;
        long second = fork();
        if (second == 0) {
            depth_value = 2;
            exit(depth_value);
        }
        int status = -1;
        waitpid(second, &status);
        exit(status == 2 && depth_value == 1 ? 3 : 1);
    }
    int status = -1;
    CHECK(waitpid(first, &status) == first);
    CHECK(status == 3);
    CHECK(depth_value == 0);

    CHECK(munmap(big, PAGES * 4096) == 0);
    CHECK(munmap(shared_status, 4096) == 0);
}

int main(void) {
    check_basics();
    check_bad_pointers();
    check_memory();
    check_fork();
    check_copy_on_write();
    check_stack_growth();
    check_files();
    check_pipes();

    if (failures == 0) {
        printf("syscheck: all checks passed\n");
        return 0;
    }
    printf("syscheck: %d check(s) failed\n", failures);
    return 1;
}
