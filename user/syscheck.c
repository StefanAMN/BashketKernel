/* Exercises the system call interface from ring 3 and exits 0 only if every
 * check passes. The kernel test suite runs this and checks the exit status. */
#include "ulib.h"

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { printf("syscheck: FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
    } while (0)

/* A program may set the direction flag. The kernel must not inherit it when
 * an interrupt arrives: its own copies would then run backwards. */
static void check_direction_flag(void) {
    long pid = fork();
    if (pid == 0) {
        asm volatile("std");
        volatile unsigned long spin = 0;
        unsigned long until = (unsigned long)uptime_ticks() + 6;
        while ((unsigned long)uptime_ticks() < until) spin++;   /* Timer interrupts and system calls with DF set */
        asm volatile("cld");
        exit(0);
    }
    int status = -1;
    CHECK(waitpid(pid, &status) == pid && status == 0);
}

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
    CHECK(execv((const char *)0xffffffff80000000, NULL) == -EFAULT);
}

static void check_basics(void) {
    CHECK(getpid() > 0);
    CHECK(write(1, "", 0) == 0);
    CHECK(write(7, "x", 1) == -EBADF);
    CHECK(syscall0(9999) == -ENOSYS);
    CHECK(uptime_ticks() >= 0);
    CHECK(execv("/bin/does-not-exist", NULL) == -ENOENT);
    CHECK(waitpid(-1, NULL) == -ECHILD);

    CHECK(set_priority(2) == 0 && set_priority(0) == 0 && set_priority(1) == 0);
    CHECK(set_priority(3) == -EINVAL && set_priority(-1) == -EINVAL);

    char cwd[64];
    CHECK(getcwd(cwd, sizeof(cwd)) == 1 && strcmp(cwd, "/") == 0);
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
        execv("/bin/hello", args);
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

static void check_directories(void) {
    char cwd[64], buf[8];

    CHECK(mkdir("/tmp/cwd-test") == 0);
    CHECK(chdir("/tmp/cwd-test") == 0);
    CHECK(getcwd(cwd, sizeof(cwd)) == 13 && strcmp(cwd, "/tmp/cwd-test") == 0);
    CHECK(getcwd(cwd, 5) == -EINVAL);                      /* Buffer too small */
    CHECK(getcwd((char *)0xffffffff80000000, 64) == -EFAULT);

    /* Relative paths start here; absolute ones do not */
    long fd = open("note", O_WRONLY | O_CREAT);
    CHECK(fd >= 0);
    CHECK(write((int)fd, "hi", 2) == 2);
    close((int)fd);
    fd = open("/tmp/cwd-test/note", O_RDONLY);
    CHECK(fd >= 0 && read((int)fd, buf, sizeof(buf)) == 2);
    close((int)fd);
    CHECK(open("/note", O_RDONLY) == -ENOENT);

    /* "." and ".." */
    CHECK(mkdir("sub") == 0);
    CHECK(chdir("./sub/../sub") == 0);
    CHECK(getcwd(cwd, sizeof(cwd)) > 0 && strcmp(cwd, "/tmp/cwd-test/sub") == 0);
    fd = open("../note", O_RDONLY);
    CHECK(fd >= 0);
    close((int)fd);
    CHECK(chdir("../../../../..") == 0);                   /* ".." stops at the root */
    CHECK(getcwd(cwd, sizeof(cwd)) == 1 && strcmp(cwd, "/") == 0);
    CHECK(mkdir("/tmp/.") == -ENOENT && mkdir("/tmp/..") == -ENOENT);
    CHECK(unlink("/tmp/cwd-test/sub/..") == -ENOENT);

    CHECK(chdir("/no/such") == -ENOENT);
    CHECK(chdir("/etc/motd") == -ENOTDIR);
    CHECK(chdir("/etc/motd/x") == -ENOENT);
    CHECK(chdir((const char *)0xffffffff80000000) == -EFAULT);

    /* A child starts in its parent's directory, and changing it there does
     * not move the parent */
    CHECK(chdir("/tmp/cwd-test/sub") == 0);
    long pid = fork();
    if (pid == 0) {
        char child_cwd[64];
        int ok = getcwd(child_cwd, sizeof(child_cwd)) > 0 && strcmp(child_cwd, "/tmp/cwd-test/sub") == 0;
        chdir("/");
        exit(ok ? 0 : 1);
    }
    int status = -1;
    CHECK(waitpid(pid, &status) == pid && status == 0);
    CHECK(getcwd(cwd, sizeof(cwd)) > 0 && strcmp(cwd, "/tmp/cwd-test/sub") == 0);

    /* A directory someone is standing in can not be removed */
    CHECK(unlink("/tmp/cwd-test/sub") == -EBUSY);
    CHECK(chdir("/") == 0);
    CHECK(unlink("/tmp/cwd-test/sub") == 0);
    CHECK(unlink("/tmp/cwd-test/note") == 0);
    CHECK(unlink("/tmp/cwd-test") == 0);
}

/* Run "sh -c <command>" with the given environment (NULL: ours) and return
 * its exit status */
static int shell_status(const char *command, char *const envp[]) {
    long pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/sh", "-c", (char *)command, NULL };
        if (envp) execve("/bin/sh", args, envp);
        else execv("/bin/sh", args);
        exit(99);
    }
    int status = -1;
    waitpid(pid, &status);
    return status;
}

static void check_environment(void) {
    /* The kernel started us with a small default environment */
    CHECK(getenv("PATH") != NULL && strcmp(getenv("PATH"), "/bin") == 0);
    CHECK(getenv("NO_SUCH_VARIABLE") == NULL);
    CHECK(getenv("PAT") == NULL);                         /* A prefix of a name is not the name */

    /* Variables we set are inherited through fork and exec ... */
    CHECK(setenv("SYSCHECK_CODE", "42") == 0);
    CHECK(strcmp(getenv("SYSCHECK_CODE"), "42") == 0);
    CHECK(shell_status("exit $SYSCHECK_CODE", NULL) == 42);
    CHECK(setenv("SYSCHECK_CODE", "43") == 0);            /* Replacing a value */
    CHECK(shell_status("exit $SYSCHECK_CODE", NULL) == 43);

    /* ... but a child's changes do not come back to the parent */
    CHECK(shell_status("export SYSCHECK_CODE=7", NULL) == 0);
    CHECK(strcmp(getenv("SYSCHECK_CODE"), "43") == 0);

    /* execve can pass a completely different environment */
    char *custom[] = { "CODE=9", "PATH=/nowhere:/bin", NULL };
    CHECK(shell_status("exit $CODE", custom) == 9);
    CHECK(shell_status("exit $SYSCHECK_CODE", custom) == 0);   /* Not set there: expands to nothing */
    CHECK(shell_status("hello > /dev/null", custom) == 0);     /* Found through the second PATH entry */
    char *nowhere[] = { "PATH=/nowhere", NULL };
    CHECK(shell_status("hello", nowhere) == 127);              /* Not on PATH: command not found */
    CHECK(shell_status("/bin/hello > /dev/null", nowhere) == 0);
    char *empty[] = { NULL };
    CHECK(shell_status("exit 5", empty) == 5);                 /* An empty environment is fine */

    /* The shell's own variables and $? */
    CHECK(shell_status("export A=3 B=4", NULL) == 0);
    CHECK(shell_status("fault", NULL) == 2);                   /* fault with no argument exits 2 */

    /* Bad pointers and oversized lists are errors, and we are still here */
    char *args[] = { "/bin/hello", NULL };
    CHECK(execve("/bin/hello", args, (char **)0xffffffff80000000) == -EFAULT);
    char *bad_entry[] = { (char *)0xffffffff80000000, NULL };
    CHECK(execve("/bin/hello", args, bad_entry) == -EFAULT);
    static char *too_many[40];
    for (int i = 0; i < 39; i++) too_many[i] = "X=1";
    CHECK(execve("/bin/hello", args, too_many) == -E2BIG);
    CHECK(execve("/bin/hello", too_many, NULL) == -E2BIG);
    static char huge[5000];
    memset(huge, 'h', sizeof(huge) - 1);
    char *one_huge[] = { huge, NULL };
    CHECK(execve("/bin/hello", args, one_huge) == -E2BIG);
}

static volatile int handler_calls = 0;
static volatile int handler_last_signal = 0;

static void counting_handler(int sig) {
    handler_calls++;
    handler_last_signal = sig;
}

/* A handler that uses the FPU and makes a system call, to check that neither
 * disturbs the code it interrupted */
static void busy_handler(int sig) {
    volatile double scratch = 3.25;
    for (int i = 0; i < 50; i++) scratch = scratch * 1.5 + (double)sig;
    getpid();
    handler_calls++;
}

/* Integer and floating point work with many live registers. The result
 * depends on every step, so a register changed behind its back shows. */
__attribute__((noinline)) static unsigned long register_churn(unsigned long seed, long rounds) {
    unsigned long a = seed, b = seed ^ 0x9E3779B97F4A7C15UL, c = 1, d = 2, e = 3, f = 5;
    double x = 0.5, y = 1.25;
    for (long i = 0; i < rounds; i++) {
        a = a * 6364136223846793005UL + b;
        b = (b << 7) | (b >> 57);
        c += a ^ d;
        d = d * 31 + e;
        e ^= f + (unsigned long)i;
        f = (f >> 3) + c;
        x = x * 3.9 * (1.0 - x);
        y += x;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ (unsigned long)(y * 1000.0);
}

static void check_signals(void) {
    int status;

    /* A handler runs, gets the signal number, and the program carries on */
    handler_calls = 0;
    CHECK(signal(SIGUSR1, counting_handler) == SIG_DFL);
    CHECK(kill(getpid(), SIGUSR1) == 0);                 /* Runs before kill() returns to us */
    CHECK(handler_calls == 1 && handler_last_signal == SIGUSR1);
    CHECK(kill(getpid(), SIGUSR1) == 0 && handler_calls == 2);

    /* Ignoring, and going back to the default */
    CHECK(signal(SIGUSR1, SIG_IGN) == counting_handler);
    CHECK(kill(getpid(), SIGUSR1) == 0 && handler_calls == 2);
    CHECK(signal(SIGUSR1, SIG_DFL) == SIG_IGN);

    /* Requests that are refused */
    CHECK(signal(SIGKILL, counting_handler) == SIG_ERR);
    CHECK(signal(0, counting_handler) == SIG_ERR);
    CHECK(signal(32, counting_handler) == SIG_ERR);
    CHECK(kill(getpid(), 32) == -ESRCH);

    /* The default action still ends a process, with the signal in its status */
    long pid = fork();
    if (pid == 0) { for (;;) yield(); }
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(waitpid(pid, &status) == pid && status == 128 + SIGTERM);

    /* SIGKILL ends a process even if it ignores everything else */
    pid = fork();
    if (pid == 0) {
        for (int s = 1; s < 32; s++) signal(s, SIG_IGN);
        for (;;) yield();
    }
    sleep_ms(20);
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(kill(pid, SIGINT) == 0);
    sleep_ms(20);
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(waitpid(pid, &status) == pid && status == 128 + SIGKILL);

    /* Handlers are inherited by fork; exec goes back to the default, because
     * the handler's code is gone with the old program */
    signal(SIGUSR2, counting_handler);
    pid = fork();
    if (pid == 0) {
        handler_calls = 0;
        kill(getpid(), SIGUSR2);
        exit(handler_calls == 1 ? 0 : 1);
    }
    CHECK(waitpid(pid, &status) == pid && status == 0);
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/spin", "sleep", NULL };
        execv("/bin/spin", args);
        exit(99);
    }
    sleep_ms(30);
    CHECK(kill(pid, SIGUSR2) == 0);
    CHECK(waitpid(pid, &status) == pid && status == 128 + SIGUSR2);
    signal(SIGUSR2, SIG_DFL);

    /* The hard part: a handler may run between any two instructions. Send a
     * computing child a stream of signals; its results must be exactly those
     * of an undisturbed run, which means every register and the FPU state
     * came back as they were. */
    unsigned long expected = register_churn(12345, 400000);
    int ends[2];
    CHECK(pipe(ends) == 0);
    pid = fork();
    if (pid == 0) {
        close(ends[0]);
        handler_calls = 0;
        signal(SIGUSR1, busy_handler);
        write(ends[1], "r", 1);                          /* Ready for signals */
        /* Keep computing until the handler has cut in 20 times (with an upper
         * bound, in case no signal ever arrives) */
        int correct = 1;
        volatile unsigned long seed = 12345; /* Volatile, or the compiler computes the result once and skips the loop */
        for (int run = 0; run < 5000 && handler_calls < 20; run++) {
            if (register_churn(seed, 400000) != expected) correct = 0;
        }
        /* 2: hardly interrupted, so the run proves little */
        exit(!correct ? 1 : (handler_calls >= 20 ? 0 : 2));
    }
    close(ends[1]);
    char ready;
    CHECK(read(ends[0], &ready, 1) == 1);
    close(ends[0]);
    for (int i = 0; i < 400; i++) {
        if (kill(pid, SIGUSR1) != 0) break;              /* It has finished */
        sleep_ms(10);
    }
    CHECK(waitpid(pid, &status) == pid);
    if (status != 0) printf("syscheck: signal flood child ended with status %d\n", status);
    CHECK(status == 0);
}

/* A signal with a handler wakes a process that is asleep in a system call:
 * the call returns -EINTR and the handler has run by then. `what` selects
 * the call. The child exits 0 if that is what it saw. */
static int interrupted_child(int what) {
    int ends[2], ready[2];
    if (pipe(ends) != 0 || pipe(ready) != 0) return -1;

    long pid = fork();
    if (pid == 0) {
        char byte;
        handler_calls = 0;
        signal(SIGUSR1, counting_handler);
        close(ready[0]);
        write(ready[1], "r", 1);                /* Handler installed: the parent may fire */
        close(ready[1]);

        long result;
        if (what == 0) {
            result = read(ends[0], &byte, 1);    /* Empty pipe, writer still open: sleeps */
        } else if (what == 1) {
            result = syscall1(SYS_SLEEP, 60000); /* A minute */
        } else {
            long grandchild = fork();
            if (grandchild == 0) { for (;;) sleep_ms(60000); }
            result = waitpid(grandchild, NULL);  /* The grandchild never exits */
            kill(grandchild, SIGKILL);
            while (waitpid(grandchild, NULL) == -EINTR) { }
        }
        exit(result == -EINTR && handler_calls == 1 ? 0 : 1);
    }

    close(ready[1]);
    char byte;
    read(ready[0], &byte, 1);
    close(ready[0]);
    sleep_ms(50);                                /* Let it get as far as going to sleep */
    long start = uptime_ticks();
    kill(pid, SIGUSR1);

    int status = -1;
    waitpid(pid, &status);
    long waited = uptime_ticks() - start;
    close(ends[0]);
    close(ends[1]);
    return (status == 0 && waited < 300) ? 0 : (status == 0 ? 100 : status); /* 100: it only woke by itself */
}

static void check_interrupted_calls(void) {
    CHECK(interrupted_child(0) == 0);            /* read on a pipe */
    CHECK(interrupted_child(1) == 0);            /* sleep */
    CHECK(interrupted_child(2) == 0);            /* waitpid */

    /* An ignored signal does not disturb a sleeper at all */
    long pid = fork();
    if (pid == 0) {
        signal(SIGUSR1, SIG_IGN);
        exit(syscall1(SYS_SLEEP, 150) == 0 ? 0 : 1);
    }
    sleep_ms(30);
    kill(pid, SIGUSR1);
    int status = -1;
    CHECK(waitpid(pid, &status) == pid && status == 0);
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
    check_direction_flag();
    check_bad_pointers();
    check_memory();
    check_fork();
    check_copy_on_write();
    check_stack_growth();
    check_files();
    check_directories();
    check_environment();
    check_signals();
    check_interrupted_calls();
    check_pipes();

    if (failures == 0) {
        printf("syscheck: all checks passed\n");
        return 0;
    }
    printf("syscheck: %d check(s) failed\n", failures);
    return 1;
}
