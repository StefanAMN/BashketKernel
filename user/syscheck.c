/* Exercises the system call interface from ring 3 and exits 0 only if every
 * check passes. The kernel test suite runs this and checks the exit status. */
#include "ulib.h"
#include "password.h"
#include "lineedit.h"
#include "glob.h"
#include "lines.h"

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { printf("syscheck: FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
    } while (0)

/* The system call process `pid` is in, from "Syscall:" in its
 * /proc/<pid>/status: its number, -1 if it is in none, -2 if there is no
 * such process (or the line is missing) */
static long syscall_of(long pid) {
    char path[40] = "/proc/", text[512];
    char digits[20];
    int d = 0;
    do { digits[d++] = (char)('0' + pid % 10); pid /= 10; } while (pid > 0);
    size_t at = 6;
    while (d > 0) path[at++] = digits[--d];
    memcpy(path + at, "/status", 8);

    long fd = open(path, O_RDONLY);
    if (fd < 0) return -2;
    long total = 0, n;
    while (total < (long)sizeof(text) - 1 && (n = read((int)fd, text + total, sizeof(text) - 1 - (size_t)total)) > 0) total += n;
    close((int)fd);
    text[total] = '\0';
    const char *line = NULL;
    for (const char *c = text; *c && !line; c++) {
        if (*c == '\n' && strncmp(c + 1, "Syscall:\t", 9) == 0) line = c + 10;
    }
    if (!line) return -2;
    if (*line == '-') return -1;
    long number = 0;
    while (*line >= '0' && *line <= '9') number = number * 10 + (*line++ - '0');
    return number;
}

/* Wait (5 seconds at most) until process `pid` is inside system call
 * `number`; whether it got there. This, not a sleep, is how a check knows
 * that a child has reached a call before signalling it: on a slow machine
 * a child can take longer than any fixed sleep to get anywhere. (A
 * handled signal that arrives once the call has begun ends it with EINTR
 * even if the child had not yet gone to sleep in it.) */
static int wait_in_syscall(long pid, long number) {
    for (int tries = 0; tries < 500; tries++) {
        if (syscall_of(pid) == number) return 1;
        sleep_ms(10);
    }
    return 0;
}

/* Wait (5 seconds at most) until `*counter` has moved past `from`: a
 * process sharing it is running. Whether it did. */
static int wait_counter_past(volatile unsigned long *counter, unsigned long from) {
    for (int tries = 0; tries < 500; tries++) {
        if (*counter > from) return 1;
        sleep_ms(10);
    }
    return 0;
}

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
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
}

/* GS belongs to the program in ring 3: the kernel switches to its own base
 * on entry (swapgs) and back on exit. Segment selector 0 is what programs
 * start with, and reading through GS must not reach kernel data. */
static void check_gs_is_ours(void) {
    unsigned long selector = 99;
    asm volatile("mov %0, gs" : "=r"(selector));
    CHECK((selector & 0xFFFF) == 0);

    /* gs:[0] in ring 3 is address 0 (our base is 0), not the kernel's
     * per-CPU structure: the access must fault, in a child */
    long pid = fork();
    if (pid == 0) {
        volatile unsigned long leaked = 0;
        asm volatile("mov %0, gs:[0]" : "=r"(leaked));
        exit(leaked ? 1 : 2);
    }
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + 11);

    /* Many crossings by every route (system call, timer interrupt, signal,
     * fork) and the kernel still finds its data: the rest of syscheck
     * passing is the evidence. A quick burst here for good measure. */
    for (int i = 0; i < 2000; i++) getpid();
    CHECK(getpid() > 0);
}

/* Pointers into the kernel, to NULL or past the user half: -EFAULT, never
 * a crash (each check_ function below counts its failed CHECKs) */
static void check_bad_pointers(void) {
    /* The kernel must refuse pointers outside our address space ... */
    CHECK(write(1, (void *)0xffffffff80000000, 16) == -EFAULT);
    CHECK(write(1, (void *)0, 1) == -EFAULT);
    CHECK(read(0, (void *)0xffff800000000000, 1) == -EFAULT);
    CHECK(syscall1(SYS_UNAME, 0xffffffff80000000) == -EFAULT);
    CHECK(waitpid(-1, (int *)0xffffffff80000000, 0) == -EFAULT);
    /* ... a range that starts valid and runs off the end of the stack ... */
    char local = 'x';
    CHECK(write(1, &local, 1UL << 40) == -EFAULT);
    /* ... a length that wraps around the address space ... */
    CHECK(write(1, &local, (size_t)-1) == -EFAULT);
    /* ... and writing into read-only memory on our behalf. */
    CHECK(read(0, (void *)(unsigned long)check_bad_pointers, 1) == -EFAULT);
    CHECK(execv((const char *)0xffffffff80000000, NULL) == -EFAULT);
}

/* Identity, unknown calls and bad descriptors, the clocks and sleeping,
 * priorities, the screen's description (fb_info) */
static void check_basics(void) {
    CHECK(getpid() > 0);
    CHECK(write(1, "", 0) == 0);
    CHECK(write(7, "x", 1) == -EBADF);
    CHECK(syscall0(9999) == -ENOSYS);
    CHECK(uptime_ticks() >= 0);

    /* The fine clock moves forward, and agrees with the tick count.
     *
     * Ticks and the fine clock are compared here, and in a virtual machine
     * neither is exact against the other for one measurement: if the host
     * stalls the machine for a moment, real time jumps ahead, and afterwards
     * the emulated timer delivers its missed ticks in a burst, so ten ticks
     * pass in less than 100 ms. One measurement in the expected window out
     * of several shows the two clocks agree; none at all would be a bug. */
    unsigned long t0 = clock_ns(), t1 = clock_ns();
    CHECK(t1 >= t0);

    int in_window = 0;
    for (int attempt = 0; attempt < 6 && !in_window; attempt++) {
        t0 = clock_ns();
        sleep_ms(100);
        unsigned long elapsed = clock_ns() - t0;
        in_window = (elapsed >= 95UL * 1000000 && elapsed <= 125UL * 1000000);
    }
    CHECK(in_window);

    /* A 2 ms sleep takes about 2 ms, not until the next 10 ms tick or two.
     * (This one is measured and timed by the same clock, so it can never
     * come out short; only "not too long" needs the best of several.) */
    unsigned long best = ~0UL;
    for (int attempt = 0; attempt < 6; attempt++) {
        t0 = clock_ns();
        CHECK(sleep_ns(2000000) == 0);
        unsigned long elapsed = clock_ns() - t0;
        CHECK(elapsed >= 2000000);
        if (elapsed < best) best = elapsed;
    }
    /* (On a machine without a timestamp counter the kernel's clock and its
     * sleeps are only good to a 10 ms tick: /proc/timers says which.) */
    char timers[160] = {0};
    long tfd = open("/proc/timers", O_RDONLY);
    CHECK(tfd >= 0);
    CHECK(read((int)tfd, timers, sizeof(timers) - 1) > 0);
    close((int)tfd);
    int one_shot = strncmp(timers, "mode: one-shot\n", 15) == 0;
    CHECK(one_shot || strncmp(timers, "mode: periodic\n", 15) == 0);
    CHECK(best < (one_shot ? 5000000UL : 30000000UL));

    /* And so does sleep(), which is given in milliseconds */
    best = ~0UL;
    for (int attempt = 0; attempt < 6; attempt++) {
        t0 = clock_ns();
        sleep_ms(3);
        unsigned long elapsed = clock_ns() - t0;
        CHECK(elapsed >= 3000000);
        if (elapsed < best) best = elapsed;
    }
    CHECK(best < (one_shot ? 6000000UL : 30000000UL));
    CHECK(sleep_ns(0) == 0);

    /* The screen as a file of pixels. Only looked at here: opening it for
     * writing would take the screen from the console (paint does that). */
    struct fb_info screen = { 0, 0, 0, 0 };
    if (fb_info(&screen) == 0) {
        CHECK(screen.width >= 320 && screen.height >= 200 && screen.bpp == 32);
        CHECK(screen.pitch >= screen.width * 4);
        CHECK(fb_info((struct fb_info *)0x10) == -EFAULT);
        CHECK(fb_info((struct fb_info *)0xFFFF800000000000UL) == -EFAULT);
        long fbfd = open("/dev/fb0", O_RDONLY);
        CHECK(fbfd >= 0);
        unsigned pixel = 0xFFFFFFFFu;
        CHECK(read((int)fbfd, &pixel, 4) == 4);
        CHECK(lseek((int)fbfd, 0, SEEK_END) == (long)screen.pitch * screen.height);
        CHECK(read((int)fbfd, &pixel, 4) == 0);                    /* The end of the screen */
        CHECK(write((int)fbfd, &pixel, 4) < 0);                    /* Opened for reading */
        close((int)fbfd);
    } else {
        CHECK(fb_info(&screen) == -ENODEV);                       /* A machine without a screen */
    }
    /* The mouse: only opened here (reading it would wait for someone to
     * move it) */
    long mousefd = open("/dev/mouse", O_RDONLY);
    if (mousefd >= 0) {
        char byte;
        CHECK(read((int)mousefd, &byte, 1) == -EINVAL);           /* Events are three bytes: no room for one */
        close((int)mousefd);
    }
    CHECK(sleep_ns(~0UL) == -EINVAL);
    CHECK(execv("/bin/does-not-exist", NULL) == -ENOENT);
    CHECK(waitpid(-1, NULL, 0) == -ECHILD);

    CHECK(set_priority(2) == 0 && set_priority(0) == 0 && set_priority(1) == 0);
    CHECK(set_priority(3) == -EINVAL && set_priority(-1) == -EINVAL);

    char cwd[64];
    CHECK(getcwd(cwd, sizeof(cwd)) == 1 && strcmp(cwd, "/") == 0);
}

/* brk and mmap: grow, use, give back, and refuse what cannot be */
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

/* fork: the child has its own copy of the stack and the globals, and its
 * exit status reaches the parent */
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
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(status == 42);
    CHECK(global_counter == 100);   /* The child's writes did not reach us */
    CHECK(stack_value == 7);
    CHECK(waitpid(pid, &status, 0) == -ECHILD); /* Already collected */

    /* fork + exec + wait: run another program and get its exit status */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/hello", "from", "syscheck", NULL };
        execv("/bin/hello", args);
        exit(99); /* Only reached if exec failed */
    }
    status = -1;
    CHECK(waitpid(-1, &status, 0) == pid);
    CHECK(status == 0);

    /* A child that crashes is killed; the parent sees a signal-style status */
    pid = fork();
    if (pid == 0) {
        volatile int *null = (int *)0;
        *null = 1;
        exit(0);
    }
    status = -1;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(status == 128 + 11);

    /* kill() ends a child that would otherwise run forever */
    pid = fork();
    if (pid == 0) {
        for (;;) yield();
    }
    CHECK(kill(pid, 9) == 0);
    CHECK(kill(pid, 9) == 0);                 /* Asking twice is harmless */
    status = -1;
    CHECK(waitpid(pid, &status, 0) == pid);
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
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(status == 128 + 15);

    /* So can one that is asleep */
    pid = fork();
    if (pid == 0) {
        for (;;) sleep_ms(100000);
    }
    sleep_ms(30);
    CHECK(kill(pid, 9) == 0);
    status = -1;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(status == 128 + 9);
}

/* open, read, write, lseek, close: errors, appending, truncation */
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

/* rename: a new name, a move into a directory, and what it refuses */
static void check_rename(void) {
    char buf[16];
    long fd = open("/tmp/first name", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "moved", 5) == 5 && close((int)fd) == 0);
    CHECK(mkdir("/tmp/a folder") == 0);

    /* A new name; the old one is gone, the contents are not */
    CHECK(rename("/tmp/first name", "/tmp/second name") == 0);
    CHECK(open("/tmp/first name", O_RDONLY) == -ENOENT);
    fd = open("/tmp/second name", O_RDONLY);
    CHECK(fd >= 0 && read((int)fd, buf, sizeof(buf)) == 5 && memcmp(buf, "moved", 5) == 0);
    close((int)fd);

    /* Into a directory, and relative to the current one */
    CHECK(rename("/tmp/second name", "/tmp/a folder/third") == 0);
    CHECK(chdir("/tmp/a folder") == 0);
    CHECK(rename("third", "../back") == 0);
    CHECK(chdir("/") == 0);
    fd = open("/tmp/back", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);

    /* Refused: nothing is replaced, nothing is invented, nothing leaves its
     * filesystem, and a directory does not go inside itself */
    CHECK(rename("/tmp/back", "/tmp/a folder") == -EEXIST);
    CHECK(rename("/tmp/no such", "/tmp/x") == -ENOENT);
    CHECK(rename("/tmp/back", "/no such dir/x") == -ENOENT);
    fd = open("/disk", O_RDONLY);                          /* Another filesystem, if a disk is attached */
    if (fd >= 0) {
        close((int)fd);
        CHECK(rename("/tmp/back", "/disk/back") == -EXDEV);
    }
    CHECK(rename("/tmp/a folder", "/tmp/a folder/inside") == -EINVAL);
    CHECK(rename((const char *)0xffffffff80000000UL, "/tmp/x") == -EFAULT);
    CHECK(rename("/tmp/back", (const char *)8) == -EFAULT);
    CHECK(rename("/tmp/back", "/tmp/back") == 0);          /* Onto itself: nothing to do */

    /* A directory moves with what is in it */
    CHECK(rename("/tmp/back", "/tmp/a folder/kept") == 0);
    CHECK(rename("/tmp/a folder", "/tmp/another") == 0);
    fd = open("/tmp/another/kept", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    CHECK(unlink("/tmp/another/kept") == 0 && unlink("/tmp/another") == 0);
}

/* mkdir, chdir, getcwd, unlink; ".", ".." and trailing slashes; names too long */
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
    CHECK(mkdir("/tmp/.") == -EINVAL && mkdir("/tmp/..") == -EINVAL);
    CHECK(unlink("/tmp/cwd-test/sub/..") == -EINVAL);

    /* A trailing slash is not part of the name; a name has at most 127 bytes */
    CHECK(mkdir("/tmp/cwd-test/slash/") == 0);
    CHECK(unlink("/tmp/cwd-test/slash/") == 0 && open("/tmp/cwd-test/slash", O_RDONLY) == -ENOENT);
    char long_name[160];
    memcpy(long_name, "/tmp/", 5);
    memset(long_name + 5, 'n', 128);
    long_name[133] = '\0';
    CHECK(mkdir(long_name) == -ENAMETOOLONG && open(long_name, O_WRONLY | O_CREAT) == -ENAMETOOLONG);
    long_name[132] = '\0';                                 /* 127: allowed */
    CHECK(mkdir(long_name) == 0 && unlink(long_name) == 0);

    CHECK(chdir("/no/such") == -ENOENT);
    CHECK(chdir("/etc/motd") == -ENOTDIR);
    CHECK(chdir("/etc/motd/x") == -ENOTDIR);
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
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
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
    waitpid(pid, &status, 0);
    return status;
}

/* The environment: the default one, setenv, and inheritance through fork
 * and exec */
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

/* A handler that counts its calls and remembers the signal */
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

/* Handlers, ignoring, default actions, kill's errors, and signals that
 * interrupt a sleeping call */
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
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGTERM);

    /* SIGKILL ends a process even if it ignores everything else */
    pid = fork();
    if (pid == 0) {
        for (int s = 1; s < 32; s++) signal(s, SIG_IGN);
        for (;;) sleep_ms(600000);                       /* In SYS_SLEEP: everything is ignored by now */
    }
    CHECK(wait_in_syscall(pid, SYS_SLEEP));             /* (A sleep here failed on a slow machine: */
    CHECK(kill(pid, SIGTERM) == 0);                      /*  SIGTERM came before the child ignored it) */
    CHECK(kill(pid, SIGINT) == 0);
    CHECK(syscall_of(pid) == SYS_SLEEP);                 /* Neither disturbed it */
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGKILL);

    /* Handlers are inherited by fork; exec goes back to the default, because
     * the handler's code is gone with the old program */
    signal(SIGUSR2, counting_handler);
    pid = fork();
    if (pid == 0) {
        handler_calls = 0;
        kill(getpid(), SIGUSR2);
        exit(handler_calls == 1 ? 0 : 1);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/spin", "sleep", NULL };
        execv("/bin/spin", args);
        exit(99);
    }
    CHECK(wait_in_syscall(pid, SYS_SLEEP));            /* spin's sleep: the exec is done (before it, the */
    CHECK(kill(pid, SIGUSR2) == 0);                     /* inherited handler would catch the signal) */
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGUSR2);
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
    CHECK(waitpid(pid, &status, 0) == pid);
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
            result = waitpid(grandchild, NULL, 0);  /* The grandchild never exits */
            kill(grandchild, SIGKILL);
            while (waitpid(grandchild, NULL, 0) == -EINTR) { }
        }
        exit(result == -EINTR && handler_calls == 1 ? 0 : 1);
    }

    close(ready[1]);
    char byte;
    read(ready[0], &byte, 1);
    close(ready[0]);
    /* In the call being tested (a signal before it would only run the
     * handler, and the call would then wait for ever) */
    if (!wait_in_syscall(pid, what == 0 ? SYS_READ : what == 1 ? SYS_SLEEP : SYS_WAITPID)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(ends[0]);
        close(ends[1]);
        return 101;                              /* 101: it never got there */
    }
    long start = uptime_ticks();
    kill(pid, SIGUSR1);

    int status = -1;
    waitpid(pid, &status, 0);
    long waited = uptime_ticks() - start;
    close(ends[0]);
    close(ends[1]);
    return (status == 0 && waited < 300) ? 0 : (status == 0 ? 100 : status); /* 100: it only woke by itself */
}

/* --- Signal masks, sigaction, SIGCHLD, restarted calls --------------------- */

static volatile int mask_calls, mask_depth, mask_deepest;
static volatile int order[8], order_count;

/* (Counts calls, for the mask checks) */
static void count_mask_calls(int sig) {
    (void)sig;
    mask_calls++;
}

static int handler_notify_fd = -1;  /* Where notify_handler reports that it ran */

/* Counts calls like count_mask_calls, and writes a byte to
 * handler_notify_fd, so that another process can tell the handler has run */
static void notify_handler(int sig) {
    (void)sig;
    mask_calls++;
    write(handler_notify_fd, "h", 1);
}

/* Sends itself the same signal again, twice, while running */
static void reentrant_handler(int sig) {
    int depth = ++mask_depth;
    if (depth > mask_deepest) mask_deepest = depth;
    if (mask_calls++ < 2) kill(getpid(), sig);
    mask_depth--;
}

/* SIGUSR1's handler sends SIGUSR2, which its mask holds back until it
 * returns: the order recorded shows that */
static void usr1_then_usr2(int sig) {
    order[order_count++] = sig;
    if (sig == SIGUSR1) {
        kill(getpid(), SIGUSR2);                  /* Held back by this handler's mask... */
        order[order_count++] = 100;               /* ...so this is recorded first */
    }
}

/* sigprocmask and sigaction's mask: blocked signals wait, SIGKILL cannot
 * be blocked, a handler's own signal is held while it runs */
static void check_signal_masks(void) {
    uint32_t set, old = 0xFFFFFFFF;
    struct sigaction action, previous;

    /* The set of blocked signals: read, add, remove, replace */
    CHECK(sigprocmask(SIG_BLOCK, NULL, &old) == 0 && old == 0);
    set = sigbit(SIGUSR1) | sigbit(SIGKILL);
    CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0 && old == 0);
    CHECK(sigprocmask(SIG_BLOCK, NULL, &old) == 0 && old == sigbit(SIGUSR1)); /* SIGKILL can not be blocked */
    CHECK(sigprocmask(7, &set, NULL) == -EINVAL);
    CHECK(sigprocmask(SIG_BLOCK, (uint32_t *)0xffffffff80000000UL, NULL) == -EFAULT);
    CHECK(sigprocmask(SIG_BLOCK, NULL, (uint32_t *)8) == -EFAULT);

    /* A blocked signal waits; it is delivered the moment it is unblocked */
    mask_calls = 0;
    CHECK(signal(SIGUSR1, count_mask_calls) != SIG_ERR);
    CHECK(kill(getpid(), SIGUSR1) == 0);
    CHECK(kill(getpid(), SIGUSR1) == 0);          /* Twice: still one pending signal */
    sleep_ms(30);
    CHECK(mask_calls == 0);
    set = sigbit(SIGUSR1);
    CHECK(sigprocmask(SIG_UNBLOCK, &set, &old) == 0 && old == sigbit(SIGUSR1));
    CHECK(mask_calls == 1);
    CHECK(sigprocmask(SIG_BLOCK, NULL, &old) == 0 && old == 0);

    /* While a handler runs, its own signal is held back: it can not
     * interrupt itself, and runs again only after it has returned */
    mask_calls = 0; mask_depth = 0; mask_deepest = 0;
    signal(SIGUSR1, reentrant_handler);
    kill(getpid(), SIGUSR1);
    CHECK(mask_calls == 3 && mask_deepest == 1);
    CHECK(sigprocmask(SIG_BLOCK, NULL, &old) == 0 && old == 0);  /* And the mask is as it was before */

    /* sigaction: a handler that also holds back another signal */
    order_count = 0;
    action.handler = usr1_then_usr2;
    action.mask = sigbit(SIGUSR2);
    action.flags = 0;
    CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
    CHECK(previous.handler == reentrant_handler && previous.mask == 0 && previous.flags == 0);
    action.mask = 0;
    CHECK(sigaction(SIGUSR2, &action, NULL) == 0);
    kill(getpid(), SIGUSR1);
    CHECK(order_count == 3 && order[0] == SIGUSR1 && order[1] == 100 && order[2] == SIGUSR2);

    CHECK(sigaction(SIGUSR1, NULL, &previous) == 0 && previous.handler == usr1_then_usr2 && previous.mask == sigbit(SIGUSR2));
    CHECK(sigaction(SIGKILL, &action, NULL) == -EINVAL);          /* Not negotiable */
    CHECK(sigaction(0, &action, NULL) == -EINVAL && sigaction(32, &action, NULL) == -EINVAL);
    action.flags = 0x80;
    CHECK(sigaction(SIGUSR1, &action, NULL) == -EINVAL);
    CHECK(sigaction(SIGUSR1, (struct sigaction *)8, NULL) == -EFAULT);
    CHECK(sigaction(SIGUSR1, NULL, (struct sigaction *)0xffffffff80000000UL) == -EFAULT);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);

    /* A blocked signal whose effect is to end the process waits too, and a
     * child starts with its parent's mask */
    set = sigbit(SIGTERM);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int unblocking[2], go[2];
    CHECK(pipe(unblocking) == 0 && pipe(go) == 0);
    long pid = fork();
    if (pid == 0) {
        close(unblocking[0]);
        close(go[1]);
        uint32_t mine = 0;
        sigprocmask(SIG_BLOCK, NULL, &mine);
        if (mine != sigbit(SIGTERM)) exit(1);
        char byte;
        read(go[0], &byte, 1);                    /* The signal arrives before this returns, and must wait */
        write(unblocking[1], "u", 1);             /* Still alive: it did wait */
        sigprocmask(SIG_UNBLOCK, &set, NULL);     /* Here it takes effect */
        exit(2);
    }
    close(unblocking[1]);
    close(go[0]);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    CHECK(kill(pid, SIGTERM) == 0);               /* Blocked in the child (inherited mask): pending */
    CHECK(write(go[1], "g", 1) == 1);
    close(go[1]);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGTERM);
    char unblocked = 0;
    CHECK(read(unblocking[0], &unblocked, 1) == 1 && unblocked == 'u');  /* Not at once: only when it unblocked */
    close(unblocking[0]);

    /* SIGCHLD: nothing by default, a handler call if one is set */
    mask_calls = 0;
    pid = fork();
    if (pid == 0) exit(0);
    CHECK(waitpid(pid, &status, 0) == pid && mask_calls == 0);
    CHECK(signal(SIGCHLD, count_mask_calls) != SIG_ERR);
    pid = fork();
    if (pid == 0) exit(0);
    while (waitpid(pid, &status, 0) == -EINTR) { }
    CHECK(status == 0 && mask_calls == 1);
    signal(SIGCHLD, SIG_DFL);

    /* SA_RESTART: the interrupted read is made again and gets the data that
     * arrives later, instead of failing with EINTR */
    for (int restart = 0; restart <= 1; restart++) {
        int p[2];
        char byte = 0;
        CHECK(pipe(p) == 0);
        int notify[2];
        CHECK(pipe(notify) == 0);
        handler_notify_fd = notify[1];
        mask_calls = 0;
        action.handler = notify_handler;
        action.mask = 0;
        action.flags = restart ? SA_RESTART : 0;
        CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
        long parent = getpid();
        pid = fork();
        if (pid == 0) {
            /* The signal once the parent is in read(), the data only once
             * its handler has run (by then the read has failed, or been
             * made again): no timing involved */
            char seen;
            if (!wait_in_syscall(parent, SYS_READ)) exit(1);
            kill((int)parent, SIGUSR1);
            if (read(notify[0], &seen, 1) != 1) exit(1);
            write(p[1], "r", 1);
            exit(0);
        }
        close(p[1]);                       /* A child that gives up then means end-of-file, not a hang */
        long n = read(p[0], &byte, 1);
        CHECK(mask_calls == 1);
        if (restart) CHECK(n == 1 && byte == 'r');
        else CHECK(n == -EINTR);
        while (waitpid(pid, &status, 0) == -EINTR) { }
        CHECK(status == 0);
        close(notify[0]);
        close(notify[1]);
        handler_notify_fd = -1;
        close(p[0]);
    }
    signal(SIGUSR1, SIG_DFL);
}

/* --- Stopping, continuing, process groups ----------------------------------- */

static volatile int tstp_calls;
static void count_tstp(int sig) { (void)sig; tstp_calls++; }

/* Process groups, the terminal's foreground group, stopping and continuing */
static void check_jobs(void) {
    int status;
    long me = getpid();

    /* Groups: a child starts in its parent's, and may be given its own */
    long my_group = getpgid(0);
    CHECK(my_group > 0 && getpgid(me) == my_group);
    CHECK(getpgid(99999) == -ESRCH);
    long pid = fork();
    if (pid == 0) {
        for (;;) sleep_ms(1000);
    }
    CHECK(getpgid(pid) == my_group);
    CHECK(setpgid(pid, 0) == 0 && getpgid(pid) == pid);
    CHECK(setpgid(pid, 77777) == -EPERM);                  /* No such group among our own */
    CHECK(setpgid(1, 0) == -EPERM || setpgid(1, 0) == 0);  /* Someone else's process: refused (unless it is ours) */
    CHECK(setpgid(99999, 0) == -EPERM);

    /* A second child joins the first one's group; a signal to the group
     * reaches both and nobody else */
    long second = fork();
    if (second == 0) {
        for (;;) sleep_ms(1000);
    }
    CHECK(setpgid(second, pid) == 0 && getpgid(second) == pid);
    CHECK(kill(-(int)pid, SIGTERM) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGTERM);
    CHECK(waitpid(second, &status, 0) == second && status == 128 + SIGTERM);
    CHECK(kill(-(int)pid, SIGTERM) == -ESRCH);             /* Nobody left in it */
    CHECK(kill(-77777, SIGTERM) == -ESRCH);

    /* SIGSTOP suspends, whatever the process is doing; waitpid can be told.
     * The child counts in memory it shares with us, so we can see whether
     * it is running. */
    volatile unsigned long *counter = mmap_shared(4096);
    CHECK(counter != NULL);
    if (!counter) return;
    *counter = 0;
    pid = fork();
    if (pid == 0) {
        for (;;) (*counter)++;
    }
    while (*counter == 0) yield();                         /* It runs */
    CHECK(waitpid(pid, &status, WNOHANG | WUNTRACED) == 0);
    CHECK(kill(pid, SIGSTOP) == 0);
    status = 0;
    CHECK(waitpid(pid, &status, WUNTRACED) == pid);
    CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    CHECK(waitpid(pid, &status, WNOHANG | WUNTRACED) == 0); /* Reported once */

    /* Stopped means not running */
    unsigned long frozen = *counter;
    sleep_ms(100);
    CHECK(*counter == frozen);

    /* SIGCONT lets it go on */
    CHECK(kill(pid, SIGCONT) == 0);
    CHECK(wait_counter_past(counter, frozen));

    /* A stop that has been asked for and cancelled before it took effect,
     * or taken back at once, leaves it running */
    CHECK(kill(pid, SIGSTOP) == 0 && kill(pid, SIGCONT) == 0);
    sleep_ms(50);
    frozen = *counter;
    CHECK(wait_counter_past(counter, frozen));

    /* A stopped process can still be killed */
    CHECK(kill(pid, SIGSTOP) == 0);
    CHECK(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGKILL);
    munmap((void *)counter, 4096);

    /* A process stopped while asleep goes back to its sleep when continued:
     * the sleep is neither cut short nor reported as interrupted */
    /* (The stop must land inside the second's sleep. If this process is held
     * up for longer than that between seeing the child in it and stopping
     * it, the child has finished: that round proves nothing, and is made
     * again rather than counted as a failure.) */
    int stopped_asleep = 0, resumed_right = 0;
    for (int round = 0; round < 3 && !stopped_asleep; round++) {
        pid = fork();
        if (pid == 0) {
            unsigned long start = clock_ns();
            long r = syscall1(SYS_SLEEP, 1000);
            unsigned long ms = (clock_ns() - start) / 1000000UL;
            exit(r == 0 && ms >= 990 ? 0 : 1);
        }
        CHECK(wait_in_syscall(pid, SYS_SLEEP));
        CHECK(kill(pid, SIGSTOP) == 0);            /* (Succeeds on an unreaped zombie too) */
        CHECK(waitpid(pid, &status, WUNTRACED) == pid);
        if (!WIFSTOPPED(status)) continue;          /* Too late: it had already exited */
        stopped_asleep = 1;
        sleep_ms(50);
        CHECK(kill(pid, SIGCONT) == 0);
        resumed_right = waitpid(pid, &status, 0) == pid && status == 0;
    }
    CHECK(stopped_asleep && resumed_right);

    /* SIGTSTP is the polite form: it stops by default, but a program may
     * ignore it or handle it. SIGSTOP may not be touched. */
    CHECK(signal(SIGSTOP, SIG_IGN) == SIG_ERR);
    uint32_t set = sigbit(SIGSTOP), old = 0;
    CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
    CHECK(sigprocmask(SIG_BLOCK, NULL, &old) == 0 && old == 0);  /* Nor blocked */
    tstp_calls = 0;
    CHECK(signal(SIGTSTP, count_tstp) != SIG_ERR);
    CHECK(kill(me, SIGTSTP) == 0 && tstp_calls == 1);            /* Handled: we are still here */
    CHECK(signal(SIGTSTP, SIG_IGN) != SIG_ERR);
    CHECK(kill(me, SIGTSTP) == 0);
    signal(SIGTSTP, SIG_DFL);
    pid = fork();
    if (pid == 0) {
        for (;;) sleep_ms(1000);
    }
    sleep_ms(20);
    CHECK(kill(pid, SIGTSTP) == 0);
    CHECK(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTSTP);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);

    /* The terminal's foreground group: set, read, put back */
    long previous = tcgetpgrp();
    CHECK(previous >= 0);
    CHECK(tcsetpgrp(my_group) == 0 && tcgetpgrp() == my_group);
    CHECK(tcsetpgrp(previous) == 0 && tcgetpgrp() == previous);
    CHECK(waitpid(-1, &status, 0x80) == -EINVAL);
}

/* --- Memory that is promised first and given later -------------------------- */

/* A number from /proc/meminfo, in kB (-1 if it is not there) */
static long meminfo(const char *label) {
    static char text[512];
    long fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    long n = read((int)fd, text, sizeof(text) - 1);
    close((int)fd);
    if (n <= 0) return -1;
    text[n] = '\0';
    size_t label_len = strlen(label);
    char *at = NULL;
    for (char *c = text; *c && !at; c++) {
        if (strncmp(c, label, label_len) == 0) at = c;
    }
    if (!at) return -1;
    at += label_len;
    while (*at == ' ') at++;
    return atoi(at);
}

/* Read every page of this program's own code and constants once. They are
 * read from the file as they are first used, and each counts as promised
 * until then: a check of "Promised" before and after would otherwise see
 * syscheck's own code arrive. Returns how many pages were touched. */
static long touch_own_program(void) {
    static char text[2048];
    long fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return -1;
    long n = read((int)fd, text, sizeof(text) - 1);
    close((int)fd);
    if (n <= 0) return -1;
    text[n] = '\0';
    long pages = 0;
    for (char *line = text; *line; ) {
        unsigned long start = 0, end = 0;
        char *c = line;
        for (; (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'); c++) start = start * 16 + (unsigned long)(*c <= '9' ? *c - '0' : *c - 'a' + 10);
        if (*c == '-') c++;
        for (; (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'); c++) end = end * 16 + (unsigned long)(*c <= '9' ? *c - '0' : *c - 'a' + 10);
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';
        if (c[0] == ' ' && strncmp(c + 5, "program", 7) == 0) {        /* "start-end rwx program" */
            for (unsigned long page = start; page < end; page += 4096, pages++) (void)*(volatile char *)page;
        }
        if (!eol) break;
        line = eol + 1;
    }
    return pages;
}

/* Mapped memory costs nothing until touched (MemFree and Promised in
 * /proc/meminfo say so), and file mappings read the file */
static void check_lazy_memory(void) {
    const size_t big = 64UL << 20;                         /* 64 MiB */
    CHECK(touch_own_program() > 0);
    long free_before = meminfo("MemFree:");
    long promised_before = meminfo("Promised:");
    CHECK(free_before > 0 && promised_before >= 0);

    /* A large mapping costs (nearly) nothing until it is used */
    unsigned char *map = mmap_anon(big);
    CHECK(map != NULL);
    if (!map) return;
    CHECK(free_before - meminfo("MemFree:") < 1024);       /* Not 65536 kB */
    CHECK(meminfo("Promised:") - promised_before == (long)(big >> 10));

    /* Touched pages appear, zero-filled, and only those are paid for */
    int nonzero = 0;
    for (size_t i = 0; i < 16; i++) {
        unsigned char *page = map + i * (4UL << 20) + 123;
        if (*page != 0) nonzero++;
        *page = (unsigned char)(i + 1);
    }
    CHECK(nonzero == 0);
    for (size_t i = 0; i < 16; i++) CHECK(map[i * (4UL << 20) + 123] == i + 1);
    long used = free_before - meminfo("MemFree:");
    CHECK(used >= 16 * 4 && used < 2048);                  /* 16 pages, plus page tables */
    CHECK(meminfo("Promised:") - promised_before == (long)(big >> 10) - 16 * 4);

    /* The kernel may be handed an untouched part as a buffer */
    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "lazy", 4) == 4);
    CHECK(read(p[0], map + (40UL << 20), 4) == 4 && memcmp(map + (40UL << 20), "lazy", 4) == 0);
    CHECK(write(p[1], map + (50UL << 20), 3) == 3);        /* Reading untouched memory: zeros */
    char three[3] = { 1, 1, 1 };
    CHECK(read(p[0], three, 3) == 3 && three[0] == 0 && three[2] == 0);
    close(p[0]);
    close(p[1]);

    /* A child gets the same promise and its own pages */
    long pid = fork();
    if (pid == 0) {
        if (map[123] != 1) exit(1);                        /* What the parent wrote */
        map[123] = 99;
        map[(60UL << 20)] = 7;                             /* A page neither had touched */
        exit(map[(60UL << 20)] == 7 ? 0 : 2);
    }
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(map[123] == 1 && map[(60UL << 20)] == 0);        /* Ours are untouched by it */

    /* Unmapping part of it: the rest stays, the hole is a bad address */
    CHECK(munmap(map + (8UL << 20), 4UL << 20) == 0);
    CHECK(map[123] == 1 && map[(12UL << 20) + 123] == 4);  /* Before and after the hole */
    CHECK(write(1, map + (8UL << 20), 1) == -EFAULT);
    pid = fork();
    if (pid == 0) {
        map[(8UL << 20) + 5] = 1;                          /* Must fault */
        exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGSEGV);

    /* Everything given back: frames and promises */
    CHECK(munmap(map, big) == 0);
    CHECK(meminfo("Promised:") == promised_before);
    long back = meminfo("MemFree:");
    CHECK(free_before - back < 256 && back - free_before < 256);

    /* More than the machine has is refused at once, not at first touch */
    CHECK(syscall4(SYS_MMAP, 0, 1UL << 40, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS) == -ENOMEM);
    CHECK(meminfo("Promised:") == promised_before);

    /* The heap works the same way */
    char *heap = sbrk(32L << 20);
    CHECK(heap != (void *)-1);
    if (heap != (void *)-1) {
        CHECK(free_before - meminfo("MemFree:") < 1024);
        heap[0] = 1;
        heap[(32UL << 20) - 1] = 2;
        CHECK(heap[0] == 1 && heap[(32UL << 20) - 1] == 2 && heap[16UL << 20] == 0);
        CHECK(sbrk(-(32L << 20)) != (void *)-1);
        CHECK(meminfo("Promised:") == promised_before);
    }
    CHECK(sbrk(1L << 29) == (void *)-1 || sbrk(-(1L << 29)) != (void *)-1); /* Half a GiB: refused, or taken back */

    /* A file as memory: its contents, read when first touched */
    static char by_read[9000];
    long fd = open("/bin/hello", O_RDONLY);
    CHECK(fd >= 0);
    long size = lseek((int)fd, 0, SEEK_END);
    CHECK(size > 8192 && lseek((int)fd, 0, SEEK_SET) == 0);
    CHECK(read((int)fd, by_read, sizeof(by_read)) == (long)sizeof(by_read));
    unsigned char *file = mmap_file((int)fd, (size_t)size, 0, PROT_READ | PROT_WRITE);
    CHECK(file != NULL);
    if (file) {
        CHECK(memcmp(file, by_read, sizeof(by_read)) == 0);
        CHECK(file[0] == 0x7F && file[1] == 'E');          /* It is an ELF file */

        /* Past the end of the file, up to the end of the page: zeros */
        size_t tail = ((size_t)size + 4095) & ~4095UL;
        int tail_nonzero = 0;
        for (size_t i = (size_t)size; i < tail; i++) if (file[i] != 0) tail_nonzero++;
        CHECK(tail_nonzero == 0);

        /* Private: our changes are ours, the file is as it was */
        file[0] = 'X';
        CHECK(lseek((int)fd, 0, SEEK_SET) == 0 && read((int)fd, by_read, 4) == 4 && by_read[0] == 0x7F);
        CHECK(munmap(file, (size_t)size) == 0);
    }

    /* From an offset; and the mapping outlives the descriptor */
    unsigned char *second = mmap_file((int)fd, 4096, 4096, PROT_READ);
    CHECK(second != NULL);
    CHECK(close((int)fd) == 0);
    if (second) {
        fd = open("/bin/hello", O_RDONLY);
        CHECK(lseek((int)fd, 4096, SEEK_SET) == 4096 && read((int)fd, by_read, 4096) == 4096);
        close((int)fd);
        CHECK(memcmp(second, by_read, 4096) == 0);
        pid = fork();
        if (pid == 0) {
            second[0] = 1;                                 /* Mapped read-only: must fault */
            exit(0);
        }
        CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGSEGV);
        CHECK(munmap(second, 4096) == 0);
    }

    /* Mappings of one file share the page cache's frame, but stay private:
     * a write gives the writer a copy (the other mapping, and the next one,
     * see the file); a write to the file is seen by the next mapping, while
     * a page already mapped keeps what it held */
    fd = open("/tmp/cached.txt", O_RDWR | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "first version", 13) == 13);
    unsigned char *writable = mmap_file((int)fd, 13, 0, PROT_READ | PROT_WRITE);
    unsigned char *readable = mmap_file((int)fd, 13, 0, PROT_READ);
    CHECK(writable != NULL && readable != NULL);
    if (writable && readable) {
        CHECK(memcmp(writable, "first version", 13) == 0 && memcmp(readable, "first version", 13) == 0);
        writable[0] = 'F';
        CHECK(writable[0] == 'F' && readable[0] == 'f');
        unsigned char *third = mmap_file((int)fd, 13, 0, PROT_READ);
        CHECK(third != NULL && third[0] == 'f');
        if (third) CHECK(munmap(third, 13) == 0);
        CHECK(lseek((int)fd, 0, SEEK_SET) == 0 && write((int)fd, "SECOND", 6) == 6);
        unsigned char *fourth = mmap_file((int)fd, 13, 0, PROT_READ);
        CHECK(fourth != NULL && memcmp(fourth, "SECONDversion", 13) == 0);
        CHECK(memcmp(readable, "first version", 13) == 0);
        if (fourth) CHECK(munmap(fourth, 13) == 0);
        CHECK(munmap(writable, 13) == 0 && munmap(readable, 13) == 0);
    }
    CHECK(close((int)fd) == 0 && unlink("/tmp/cached.txt") == 0);
    CHECK(meminfo("Cached:") >= 0);

    /* A mapped file is in use: it can not be removed until it is unmapped */
    fd = open("/tmp/mapped.txt", O_RDWR | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "mapped text", 11) == 11);
    char *text = mmap_file((int)fd, 11, 0, PROT_READ);
    CHECK(text != NULL && close((int)fd) == 0);
    if (text) {
        CHECK(memcmp(text, "mapped text", 11) == 0);
        CHECK(unlink("/tmp/mapped.txt") == -EBUSY);
        CHECK(munmap(text, 11) == 0);
    }
    CHECK(unlink("/tmp/mapped.txt") == 0);

    /* What can not be mapped */
    fd = open("/bin/hello", O_RDONLY);
    CHECK(syscall6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 100) == -EINVAL);       /* Offset not on a page boundary */
    CHECK(syscall6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == -EACCES); /* Writing the file needs it open for writing */
    CHECK(syscall6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, 99, 0) == -EBADF);
    CHECK(syscall6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, 1, 0) == -EACCES);          /* The console is not a file */
    CHECK(syscall6(SYS_MMAP, 0, 4096, 0x80, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == -EINVAL);
    CHECK(syscall6(SYS_MMAP, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == -EINVAL);
    close((int)fd);
    fd = open("/tmp", O_RDONLY);
    CHECK(syscall6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 0) == -EACCES);         /* Nor is a directory */
    close((int)fd);
    CHECK(meminfo("Promised:") == promised_before);
}

static long read_small(const char *path, char *buf, size_t size);
static int has_text(const char *text, const char *part);

/* In a child put in a background group (job control on, this process's
 * group in the foreground): read the terminal in the way `how` says, and
 * exit with what the read returned (as a positive number: -EIO is 5,
 * a byte written 100 + 1). how: 0 plain, 1 SIGTTIN ignored, 2 SIGTTIN
 * blocked, 3 a write instead of a read */
static void background_terminal_child(int how) {
    setpgid(0, 0);
    if (how == 1) signal(SIGTTIN, SIG_IGN);
    if (how == 2) {
        uint32_t set = sigbit(SIGTTIN);
        sigprocmask(SIG_BLOCK, &set, NULL);
    }
    char byte = '\n';
    long n = how == 3 ? write(1, &byte, 1) : read(0, &byte, 1);
    exit((int)(n < 0 ? -n : 100 + n));
}

/* Start background_terminal_child(how) and wait for it to stop or exit:
 * its waitpid status (it is killed if it stopped). A child that does
 * neither within 5 s (it sits reading the keyboard: the terminal let a
 * background job through) is killed too, and -1 returned: a failure, not
 * a hang. */
static int background_terminal_status(int how) {
    long pid = fork();
    if (pid == 0) background_terminal_child(how);
    setpgid(pid, pid);                                     /* (Both sides, as a shell does) */
    int status = -1;
    long r = 0;
    for (int tries = 0; tries < 500 && r == 0; tries++) {
        r = waitpid(pid, &status, WNOHANG | WUNTRACED);
        if (r == 0) sleep_ms(10);
    }
    if (r == 0 || WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        int ignored;
        waitpid(pid, &ignored, 0);
    }
    return r == 0 ? -1 : status;
}

/* Whether process `pid` has ended: gone from /proc, or a zombie there */
static int process_ended(long pid) {
    static char text[512];
    char path[40] = "/proc/";
    char digits[16];
    int d = 0;
    do { digits[d++] = (char)('0' + pid % 10); pid /= 10; } while (pid > 0);
    size_t at = 6;
    while (d > 0) path[at++] = digits[--d];
    memcpy(path + at, "/status", 8);
    long n = read_small(path, text, sizeof(text));
    return n == -ENOENT || (n > 0 && has_text(text, "State:\tzombie"));
}

/* Sessions and the terminal: setsid, getsid, tcsetpgrp's rules; a
 * background job that reads the terminal stops (SIGTTIN), or gets EIO if
 * SIGTTIN is ignored or blocked or its group is orphaned; one that writes
 * stops only with tostop (SIGTTOU) */
static void check_sessions_and_terminal(void) {
    CHECK(getsid(0) == 0);                                  /* The console's session */
    CHECK(setsid() == -EPERM);                              /* This process leads its group */
    CHECK(getsid(999999) == -ESRCH);
    int go[2];
    CHECK(pipe(go) == 0);
    long pid = fork();
    if (pid == 0) {
        close(go[1]);
        long sid = setsid();
        int ok = sid == getpid() && getsid(0) == sid && getpgid(0) == sid;
        ok = ok && tcsetpgrp(getpgid(0)) == -ENOTTY;       /* No terminal in a new session */
        ok = ok && setsid() == -EPERM;                     /* Now a group leader */
        char byte;
        read(go[0], &byte, 1);                             /* Until the parent has tried setpgid */
        exit(ok ? 0 : 1);
    }
    close(go[0]);
    CHECK(wait_in_syscall(pid, SYS_READ));
    CHECK(getsid(pid) == pid);
    CHECK(setpgid(pid, getpgid(0)) != 0);                  /* Not across sessions */
    CHECK(tcsetpgrp(pid) == -EPERM);                       /* Nor give the terminal to another session's group */
    CHECK(write(go[1], "x", 1) == 1);
    close(go[1]);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);

    /* Job control on: this process's group has the terminal */
    long tostop_before = tty_tostop(-1);
    CHECK(tcsetpgrp(getpgid(0)) == 0 && tcgetpgrp() == getpgid(0));
    status = background_terminal_status(0);
    CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN);
    CHECK(background_terminal_status(1) == EIO);           /* SIGTTIN ignored: the read fails */
    CHECK(background_terminal_status(2) == EIO);           /* ...or blocked */
    CHECK(tty_tostop(0) == 0 && background_terminal_status(3) == 101);   /* Writes go through... */
    CHECK(tty_tostop(1) == 0 && tty_tostop(-1) == 1);
    status = background_terminal_status(3);
    CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTOU);              /* ...unless tostop is set */
    CHECK(tty_tostop(0) == 0);
    /* An orphaned group (its parent ended) is not stopped: the read fails */
    CHECK(pipe(go) == 0);
    pid = fork();
    if (pid == 0) {
        long parent = getpid();
        long grandchild = fork();
        if (grandchild == 0) {
            close(go[0]);
            setpgid(0, 0);
            long self = getpid();
            write(go[1], &self, sizeof(self));             /* So that it can be killed if it is stopped */
            for (int tries = 0; tries < 500 && !process_ended(parent); tries++) sleep_ms(10);   /* Until it has */
            char byte, result = 'x';
            if (read(0, &byte, 1) == -EIO) result = 'E';
            write(go[1], &result, 1);
            exit(0);
        }
        exit(0);                                           /* The grandchild's group is orphaned from here on */
    }
    close(go[1]);
    long grandchild = 0;
    CHECK(read(go[0], &grandchild, sizeof(grandchild)) == (long)sizeof(grandchild));
    char result = 0;
    struct pollfd answer = { go[0], POLLIN, 0 };
    /* (A bounded wait: if the read stopped the grandchild, as it must not,
     * this fails rather than hangs) */
    CHECK(poll(&answer, 1, 5000) == 1 && read(go[0], &result, 1) == 1 && result == 'E');
    if (result != 'E' && grandchild > 0) kill(grandchild, SIGKILL);
    close(go[0]);
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(tty_tostop(tostop_before) == 0);
    CHECK(tcsetpgrp(0) == 0);                              /* Job control off again, as the kernel shell has it */
}

/* Shared mappings of the file at `path` (MAP_SHARED): one copy of each
 * page for every mapping, read() and write(); what is written reaches the
 * file; a truncation cuts pages off (SIGBUS) */
static void check_shared_mapping_of(const char *path) {
    long fd = open(path, O_RDWR | O_CREAT | O_TRUNC);
    CHECK(fd >= 0);
    if (fd < 0) return;
    static char page[4096];
    memset(page, 'a', sizeof(page));
    CHECK(write((int)fd, page, 4096) == 4096 && write((int)fd, page, 4096) == 4096 && write((int)fd, "tail", 4) == 4);
    char *map = mmap_file_shared((int)fd, 4 * 4096, 0, PROT_READ | PROT_WRITE);   /* The 4th page: past the end */
    CHECK(map != NULL);
    if (!map) { close((int)fd); unlink(path); return; }
    CHECK(map[0] == 'a' && map[8192] == 't' && map[8195] == 'l' && map[8196] == 0);   /* Past the end of the file: zeros */

    /* A page wholly past the end of the file is not the file's: SIGBUS */
    int status = -1;
    long pid = fork();
    if (pid == 0) {
        volatile char c = map[3 * 4096];
        (void)c;
        exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGBUS);

    /* A child writes through the mapping it inherited: the parent sees it */
    pid = fork();
    if (pid == 0) {
        memcpy(map + 10, "child", 5);
        exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(memcmp(map + 10, "child", 5) == 0);

    /* read() sees what was written through a mapping, before any msync;
     * write() shows in the mapping at once */
    char buf[8];
    CHECK(lseek((int)fd, 10, SEEK_SET) == 10 && read((int)fd, buf, 5) == 5 && memcmp(buf, "child", 5) == 0);
    memcpy(map + 40, "own", 3);                            /* (Not written back by anyone yet) */
    CHECK(lseek((int)fd, 40, SEEK_SET) == 40 && read((int)fd, buf, 3) == 3 && memcmp(buf, "own", 3) == 0);
    CHECK(lseek((int)fd, 4096, SEEK_SET) == 4096 && write((int)fd, "write", 5) == 5);
    CHECK(memcmp(map + 4096, "write", 5) == 0);

    /* Another mapping of its own, made afresh in another process, has the
     * same pages; what it writes is seen here (and was written to the file
     * when it exited) */
    pid = fork();
    if (pid == 0) {
        long again = open(path, O_RDWR);
        char *other = again >= 0 ? mmap_file_shared((int)again, 4096, 0, PROT_READ | PROT_WRITE) : NULL;
        if (!other || memcmp(other + 10, "child", 5) != 0) exit(1);
        memcpy(other + 20, "second", 6);
        exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(memcmp(map + 20, "second", 6) == 0);

    /* A read-only shared mapping follows the file too, and cannot be written */
    long reader = open(path, O_RDONLY);
    char *view = reader >= 0 ? mmap_file_shared((int)reader, 4096, 0, PROT_READ) : NULL;
    CHECK(view != NULL);
    if (view) {
        memcpy(map + 30, "seen", 4);
        CHECK(memcmp(view + 30, "seen", 4) == 0);
        pid = fork();
        if (pid == 0) {
            view[0] = 'x';                                 /* Faults: SIGSEGV */
            exit(0);
        }
        CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGSEGV);
        CHECK(munmap(view, 4096) == 0);
    }
    close((int)reader);

    CHECK(msync(map, 4 * 4096, MS_SYNC) == 0);
    CHECK(msync(map, 4096, MS_ASYNC | MS_INVALIDATE) == 0);
    CHECK(msync(map + 1, 4096, MS_SYNC) == -EINVAL);        /* Not on a page boundary */
    CHECK(msync(map, 4096, MS_SYNC | MS_ASYNC) == -EINVAL);
    CHECK(msync(map, 4096, 0x40) == -EINVAL);
    CHECK(msync((void *)0x10000000000UL, 4096, MS_SYNC) == -ENOMEM);   /* Nothing mapped there */
    CHECK(msync(map, 0, MS_SYNC) == 0);

    /* Truncation: pages past the new end are not the file's. A process that
     * had one mapped loses it at its next safe point (here, the return of
     * the read it waits in), and touching it is SIGBUS; so does the one that
     * truncated (here, at the return of ftruncate: a child it starts has
     * nothing there). The page the file ends in stays. */
    CHECK(map[4096] == 'w');                               /* Present here, so in the children too */
    int go[2];
    CHECK(pipe(go) == 0);
    pid = fork();
    if (pid == 0) {
        close(go[1]);
        volatile char c = map[4096];                       /* Mapped in this process too */
        (void)c;
        read(go[0], buf, 1);                               /* Until the file has been cut */
        c = map[4096];
        exit(0);
    }
    close(go[0]);
    CHECK(ftruncate((int)fd, 100) == 0);
    CHECK(write(go[1], "x", 1) == 1);
    close(go[1]);
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGBUS);
    pid = fork();
    if (pid == 0) {
        volatile char c = map[8192];
        (void)c;
        exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGBUS);
    CHECK(memcmp(map + 10, "child", 5) == 0 && map[100] == 0 && map[4095] == 0);   /* Past 100: zeros */

    /* munmap writes it to the file: read back with no mapping left */
    memcpy(map, "unmapped", 8);
    CHECK(munmap(map, 4 * 4096) == 0);
    CHECK(lseek((int)fd, 0, SEEK_SET) == 0 && read((int)fd, buf, 8) == 8 && memcmp(buf, "unmapped", 8) == 0);
    close((int)fd);
    CHECK(unlink(path) == 0);
}

/* Whether the root is the ext2 system disk (/proc/mounts says "/ ext2"):
 * the rescue system has none, and its /root is in memory, if there */
static int root_on_disk(void) {
    static char mounts[512];
    long fd = open("/proc/mounts", O_RDONLY);
    long n = fd >= 0 ? read((int)fd, mounts, sizeof(mounts) - 1) : 0;
    if (fd >= 0) close((int)fd);
    mounts[n > 0 ? n : 0] = '\0';
    for (long i = 0; i + 7 < n; i++) if (memcmp(mounts + i, " / ext2", 7) == 0) return 1;
    return 0;
}

/* The screen mapped into memory (/dev/fb0, S1): only through a descriptor
 * that may write it, shared, within its size; what is written through the
 * mapping is the screen (read back through the file) */
static void check_screen_mapping(void) {
    struct fb_info screen;
    if (fb_info(&screen) != 0) return;                     /* No screen on this machine */
    size_t bytes = (size_t)screen.pitch * screen.height;
    long fd = open("/dev/fb0", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(mmap_file_shared((int)fd, 4096, 0, PROT_READ | PROT_WRITE) == NULL);   /* Opened to read only */
    close((int)fd);
    fd = open("/dev/fb0", O_RDWR);                         /* The screen is ours from here */
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(syscall6(SYS_MMAP, 0, bytes + 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == -EINVAL);   /* Past its end */
    CHECK(syscall6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 0) == -EINVAL);  /* A device's own pages, not a copy */
    unsigned *pixels = mmap_file_shared((int)fd, bytes, 0, PROT_READ | PROT_WRITE);
    CHECK(pixels != NULL);
    if (pixels) {
        *(unsigned *)((char *)pixels + screen.pitch + 5 * 4) = 0x00ABCDEFu;   /* Row 1, column 5 (rows are pitch bytes apart) */
        unsigned seen = 0;
        CHECK(lseek((int)fd, (long)screen.pitch + 5 * 4, SEEK_SET) == (long)screen.pitch + 20);
        CHECK(read((int)fd, &seen, 4) == 4 && seen == 0x00ABCDEFu);
        /* The mapping holds the screen as the descriptor does: with only the
         * mapping left, the console does not take it back and paint over
         * the pixel (it would within a refresh, 1/50 s; 0.3 s is given) */
        close((int)fd);
        *(unsigned *)((char *)pixels + screen.pitch + 5 * 4) = 0x00FEDCBAu;
        sleep_ms(300);
        long reader = open("/dev/fb0", O_RDONLY);
        seen = 0;
        CHECK(reader >= 0 && lseek((int)reader, (long)screen.pitch + 5 * 4, SEEK_SET) > 0 &&
              read((int)reader, &seen, 4) == 4 && seen == 0x00FEDCBAu);
        if (reader >= 0) close((int)reader);
        CHECK(munmap(pixels, bytes) == 0);                 /* Now the console has it back */
    } else {
        close((int)fd);
    }
}

/* Shared file mappings, in memory and (with a system disk) on ext2, and
 * what cannot be mapped shared */
static void check_shared_mappings(void) {
    long promised_before = meminfo("Promised:");
    check_shared_mapping_of("/tmp/shared-map");
    if (root_on_disk()) check_shared_mapping_of("/root/shared-map");
    long fd = open("/dev/zero", O_RDWR);
    CHECK(fd >= 0 && syscall6(SYS_MMAP, 0, 4096, PROT_READ, MAP_SHARED, fd, 0) == -ENODEV);   /* A device: no pages to share */
    close((int)fd);
    CHECK(meminfo("Promised:") == promised_before);
}

/* A signal ends a sleeping read, sleep or waitpid with -EINTR, and an
 * ignored one does not */
static void check_interrupted_calls(void) {
    CHECK(interrupted_child(0) == 0);            /* read on a pipe */
    CHECK(interrupted_child(1) == 0);            /* sleep */
    CHECK(interrupted_child(2) == 0);            /* waitpid */

    /* An ignored signal does not disturb a sleeper at all: the read goes
     * on to get its byte, rather than failing with EINTR */
    int go[2];
    CHECK(pipe(go) == 0);
    long pid = fork();
    if (pid == 0) {
        close(go[1]);
        signal(SIGUSR1, SIG_IGN);
        char byte = 0;
        exit(read(go[0], &byte, 1) == 1 && byte == 'g' ? 0 : 1);
    }
    close(go[0]);
    CHECK(wait_in_syscall(pid, SYS_READ));       /* Asleep in it, the signal already ignored */
    kill(pid, SIGUSR1);
    sleep_ms(50);                                 /* Time for a wrongly woken read to fail (no false */
    CHECK(write(go[1], "g", 1) == 1);             /* alarm if it is longer: the byte is read either way) */
    close(go[1]);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
}

#define OUR_IP 0x0A00020FUL    /* 10.0.2.15 */

/* TCP: a server in a child process, a client here, over the loopback
 * address. The server sends back what it gets in upper case. */
static void check_tcp(void) {
    static char out[1000], in[1000];
    unsigned port = 9100 + (unsigned)(getpid() % 500);
    uint32_t loopback = parse_ip("127.0.0.1");

    CHECK(socket(99) == -EINVAL);

    /* Nobody listens yet: refused, and the socket is spent */
    long refused = socket(SOCK_STREAM);
    CHECK(refused >= 3);
    CHECK(connect((int)refused, loopback, port) == -ECONNREFUSED);
    CHECK(connect((int)refused, loopback, port) == -EISCONN);
    CHECK(close((int)refused) == 0);

    long server = socket(SOCK_STREAM);
    CHECK(server >= 3);
    CHECK(listen((int)server, 2) == -EINVAL);             /* No port yet */
    CHECK(bind((int)server, 0) == -EINVAL);
    CHECK(bind((int)server, port) == 0);
    CHECK(listen((int)server, 2) == 0);
    CHECK(read((int)server, in, 1) == -ENOTCONN);         /* A listening socket carries no data */
    CHECK(write((int)server, "x", 1) == -ENOTCONN);
    CHECK(shutdown((int)server) == -ENOTCONN);
    CHECK(sendto((int)server, "x", 1, loopback, port) == -EBADF);  /* Datagram calls are for datagram sockets */
    CHECK(accept((int)server, (struct net_address *)0xffffffff80000000UL) == -EFAULT);
    CHECK(accept(1, NULL) == -EBADF && connect(1, loopback, port) == -EBADF && listen(1, 1) == -EBADF);

    long pid = fork();
    if (pid == 0) {
        struct net_address from;
        long conn = accept((int)server, &from);
        if (conn < 0 || from.ip != loopback || from.port < 49152) exit(1);
        long n, total = 0;
        while ((n = read((int)conn, in, sizeof(in))) > 0) {
            for (long i = 0; i < n; i++) if (in[i] >= 'a' && in[i] <= 'z') in[i] -= 32;
            for (long sent = 0; sent < n; ) {
                long w = write((int)conn, in + sent, (size_t)(n - sent));
                if (w <= 0) exit(2);
                sent += w;
            }
            total += n;
        }
        if (n < 0) exit(3);
        close((int)conn);                                 /* After the client finished: we close second */
        exit(total == 20000 ? 0 : 4);
    }

    long client = socket(SOCK_STREAM);
    CHECK(client >= 3);
    CHECK(connect((int)client, loopback, port) == 0);

    /* 20 000 bytes there and back, a piece at a time */
    int mismatches = 0;
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 1000; i++) out[i] = (char)('a' + (i + round) % 26);
        CHECK(write((int)client, out, sizeof(out)) == (long)sizeof(out));
        long got = 0;
        while (got < 1000) {
            long n = read((int)client, in + got, (size_t)(1000 - got));
            if (n <= 0) break;
            got += n;
        }
        CHECK(got == 1000);
        for (int i = 0; i < 1000; i++) if (in[i] != out[i] - 32) mismatches++;
    }
    CHECK(mismatches == 0);

    /* We finish: the server sees end-of-file and closes, and so do we */
    CHECK(shutdown((int)client) == 0);
    CHECK(write((int)client, "late", 4) == -EPIPE);
    CHECK(read((int)client, in, sizeof(in)) == 0);
    CHECK(close((int)client) == 0);

    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(close((int)server) == 0);
}

/* The interface's settings, and the DNS message code in the C library
 * (checked on messages written out by hand: no server is involved) */
static void check_netconfig_and_dns(void) {
    struct net_config config;
    memset(&config, 0xEE, sizeof(config));
    CHECK(netconfig(&config, NULL) == 0);
    CHECK(config.ip == OUR_IP && config.netmask == 0xFFFFFF00u);
    CHECK(config.gateway == parse_ip("10.0.2.2") && config.dns == parse_ip("10.0.2.3"));
    CHECK(config.mac[0] != 0xEE || config.mac[1] != 0xEE);
    CHECK(netconfig(NULL, NULL) == 0);
    CHECK(netconfig((struct net_config *)0xffffffff80000000UL, NULL) == -EFAULT);
    CHECK(netconfig(NULL, (struct net_config *)8) == -EFAULT);

    /* Root may set them; nonsense is refused; the same settings again are fine */
    struct net_config bad = config;
    bad.netmask = 0xFF00FF00u;
    CHECK(netconfig(NULL, &bad) == -EINVAL);
    CHECK(netconfig(NULL, &config) == 0);

    /* A question */
    static uint8_t query[64];
    static const uint8_t expected[] = { 0xAB, 0xCD, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                        1, 'a', 2, 'b', 'c', 0, 0, 1, 0, 1 };
    CHECK(dns_build_query(query, sizeof(query), 0xABCD, "a.bc") == (long)sizeof(expected));
    CHECK(memcmp(query, expected, sizeof(expected)) == 0);
    CHECK(dns_build_query(query, sizeof(query), 0xABCD, "a.bc.") == (long)sizeof(expected)); /* A final dot changes nothing */
    CHECK(dns_build_query(query, sizeof(query), 1, "") == -EINVAL);
    CHECK(dns_build_query(query, sizeof(query), 1, "a..b") == -EINVAL);
    CHECK(dns_build_query(query, sizeof(query), 1, ".a") == -EINVAL);
    CHECK(dns_build_query(query, 20, 1, "a.bc") == -EINVAL);                      /* Does not fit */
    CHECK(dns_build_query(query, sizeof(query), 1,
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b") == -EINVAL); /* A 64-character label */

    /* An answer: the name asked about is an alias, whose address follows.
     * Names in the answers are pointers back into the question. */
    static const uint8_t reply[] = {
        0x12, 0x34, 0x81, 0x80, 0, 1, 0, 2, 0, 0, 0, 0,
        3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1,
        0xC0, 0x0C, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 0x10,                    /* www is an alias of example.com */
        0xC0, 0x10, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 93, 184, 216, 34,              /* whose address is this */
    };
    static uint8_t copy[sizeof(reply)];
    uint32_t ip = 0;
    CHECK(dns_parse_reply(reply, sizeof(reply), 0x1234, &ip) == 0);
    CHECK(ip == parse_ip("93.184.216.34"));

    CHECK(dns_parse_reply(reply, sizeof(reply), 0x1235, &ip) == -EINVAL);         /* Someone else's answer */
    CHECK(dns_parse_reply(reply, 11, 0x1234, &ip) == -EINVAL);                    /* Not even a header */
    for (size_t cut = 12; cut < sizeof(reply); cut++) {                           /* Cut short anywhere: never read past the end */
        long r = dns_parse_reply(reply, cut, 0x1234, &ip);
        CHECK(r == -EINVAL || (r == -ENOENT && cut >= 47));
    }

    memcpy(copy, reply, sizeof(reply));
    copy[2] = 0x01;                                                               /* A question, not an answer */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) == -EINVAL);
    copy[2] = 0x81; copy[3] = 0x83;                                               /* "No such name" */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) == -ENOENT);
    copy[3] = 0x82;                                                               /* "Server failure" */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) == -EINVAL);

    memcpy(copy, reply, sizeof(reply));
    copy[7] = 1;                                                                  /* Only the alias: no address */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) == -ENOENT);
    copy[7] = 9;                                                                  /* Promises more answers than there are */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) == 0);                 /* ...but the address comes first */
    copy[50] = 28;                                                                /* The address record is some other type */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) == -EINVAL);
    memcpy(copy, reply, sizeof(reply));
    copy[12] = 0x40;                                                              /* A label type that does not exist */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) == -EINVAL);
    copy[12] = 0xC0; copy[13] = 0x0C;                                             /* A name that points at itself */
    CHECK(dns_parse_reply(copy, sizeof(copy), 0x1234, &ip) != 0);

    /* An address needs no server */
    CHECK(resolve("10.1.2.3", &ip) == 0 && ip == parse_ip("10.1.2.3"));
}

/* Address parsing, UDP over loopback and to the host, ping */
static void check_network(void) {
    char buf[64];
    struct net_address from;

    /* Address parsing in the library */
    CHECK(parse_ip("10.0.2.15") == OUR_IP);
    CHECK(parse_ip("255.255.255.255") == 0xFFFFFFFFUL);
    CHECK(parse_ip("10.0.2") == 0 && parse_ip("10.0.2.256") == 0 && parse_ip("10.0.2.15x") == 0);
    CHECK(parse_ip("") == 0 && parse_ip("a.b.c.d") == 0 && parse_ip("1..2.3") == 0);
    format_ip(OUR_IP, buf);
    CHECK(strcmp(buf, "10.0.2.15") == 0);

    CHECK(ping(OUR_IP, 500) >= 0);                        /* Ourselves: always answers */

    /* A server in a child process, a client here, talking over UDP */
    long server = socket(SOCK_DGRAM);
    CHECK(server >= 3);
    CHECK(bind((int)server, 9000) == 0);
    CHECK(bind((int)server, 9001) == -EINVAL);            /* Already bound */

    long pid = fork();
    if (pid == 0) {
        /* The child inherited the socket: it answers one datagram, upper-cased */
        long n = recvfrom((int)server, buf, sizeof(buf), &from, 3000);
        if (n <= 0) exit(1);
        for (long i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        exit(sendto((int)server, buf, (size_t)n, from.ip, from.port) == n ? 0 : 2);
    }

    long client = socket(SOCK_DGRAM);
    CHECK(client >= 3 && client != server);
    CHECK(bind((int)client, 9000) == -EADDRINUSE);        /* The server has it */
    CHECK(sendto((int)client, "shout", 5, OUR_IP, 9000) == 5);
    memset(buf, 0, sizeof(buf));
    memset(&from, 0, sizeof(from));
    CHECK(recvfrom((int)client, buf, sizeof(buf), &from, 3000) == 5);
    CHECK(memcmp(buf, "SHOUT", 5) == 0);
    CHECK(from.ip == OUR_IP && from.port == 9000);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);

    /* Timeouts, limits and bad arguments */
    CHECK(recvfrom((int)client, buf, sizeof(buf), NULL, 30) == -ETIMEDOUT);
    /* One larger than a frame travels in fragments and arrives whole */
    static char big[65508], big_back[65508];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i * 7 + i / 251);
    CHECK(sendto((int)client, big, 8000, OUR_IP, 9000) == 8000);
    CHECK(recvfrom((int)server, big_back, sizeof(big_back), NULL, 500) == 8000);
    CHECK(memcmp(big, big_back, 8000) == 0);
    CHECK(sendto((int)client, big, 65507, OUR_IP, 9000) == 65507);  /* The largest datagram */
    CHECK(sendto((int)client, big, 65508, OUR_IP, 9000) == -EMSGSIZE);
    CHECK(sendto((int)client, big, 10, OUR_IP, 0) == -EINVAL);     /* Port 0 */
    CHECK(sendto((int)client, (void *)0xffffffff80000000, 10, OUR_IP, 9000) == -EFAULT);
    CHECK(recvfrom((int)server, (void *)0xffffffff80000000, 10, NULL, 30) == -EFAULT);
    CHECK(recvfrom((int)server, buf, 10, (struct net_address *)0xffffffff80000000, 30) == -EFAULT);
    CHECK(recvfrom((int)server, buf, sizeof(buf), NULL, 500) == 64);   /* The big datagram, cut to our buffer */

    /* Socket calls on things that are not sockets, and file calls on sockets */
    CHECK(sendto(1, "x", 1, OUR_IP, 9000) == -EBADF);
    CHECK(bind(0, 9100) == -EBADF);
    CHECK(recvfrom(99, buf, 1, NULL, 10) == -EBADF);
    CHECK(read((int)client, buf, 1) == -EINVAL);
    CHECK(write((int)client, "x", 1) == -EINVAL);

    /* Closing a socket frees its port */
    CHECK(close((int)server) == 0);
    CHECK(close((int)client) == 0);
    long again = socket(SOCK_DGRAM);
    CHECK(bind((int)again, 9000) == 0);
    close((int)again);

    /* A receive that would wait for ever is cut short by a handled signal */
    long waiting = fork();
    if (waiting == 0) {
        long s = socket(SOCK_DGRAM);
        bind((int)s, 9200);
        signal(SIGUSR1, counting_handler);
        exit(recvfrom((int)s, buf, sizeof(buf), NULL, 0) == -EINTR ? 0 : 1);
    }
    CHECK(wait_in_syscall(waiting, SYS_RECVFROM));       /* The handler is set by then */
    kill(waiting, SIGUSR1);
    CHECK(waitpid(waiting, &status, 0) == waiting && status == 0);
}

/* MAP_SHARED and shmget/shmat: writes are seen across fork and between
 * processes; keys and permissions */
static void check_shared_memory(void) {
    int status;

    /* mmap(MAP_SHARED): parent and child see each other's writes, unlike
     * ordinary (private) memory, which fork separates */
    volatile int *shared = mmap_shared(4096);
    volatile int *private = mmap_anon(4096);
    CHECK(shared != NULL && private != NULL);
    shared[0] = 1;
    private[0] = 1;
    long pid = fork();
    if (pid == 0) {
        shared[0] = 42;
        shared[1] = (int)getpid();
        private[0] = 42;
        exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(shared[0] == 42 && shared[1] == (int)pid);      /* The child's writes arrived */
    CHECK(private[0] == 1);                               /* ... but not in private memory */
    shared[0] = 7;                                        /* And we can still write it */
    CHECK(shared[0] == 7);
    CHECK(munmap((void *)shared, 4096) == 0 && munmap((void *)private, 4096) == 0);
    CHECK(syscall4(SYS_MMAP, 0, 4096, PROT_READ, MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS) == -EINVAL);

    /* A keyed segment: two processes that attach separately share it */
    long id = shmget(0xB45C, 8192);
    CHECK(id >= 0);
    CHECK(shmget(0xB45C, 100) == id);                     /* The same key finds the same segment */
    CHECK(shmget(0xB45C, 1 << 20) == -EINVAL);            /* Larger than it is */
    CHECK(shmget(0xB45D, 0) == -EINVAL);
    volatile char *mine = shmat(id);
    CHECK(mine != NULL);
    CHECK(mine[0] == 0 && mine[8191] == 0);               /* Starts zeroed */
    mine[0] = 'p';

    pid = fork();
    if (pid == 0) {
        /* Attach again in the child, at a different address, and answer */
        long same = shmget(0xB45C, 8192);
        volatile char *theirs = shmat(same);
        int ok = theirs != NULL && theirs != mine && theirs[0] == 'p';
        if (ok) theirs[8191] = 'c';
        exit(ok ? 0 : 1);
    }
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(mine[8191] == 'c');

    /* Removing the key does not pull the memory out from under us */
    CHECK(shmrm(id) == 0);
    CHECK(shmrm(id) == -EINVAL);
    CHECK(shmat(id) == NULL);
    mine[1] = 'x';
    CHECK(mine[0] == 'p' && mine[1] == 'x');
    CHECK(munmap((void *)mine, 8192) == 0);
    CHECK(write(1, (void *)mine, 1) == -EFAULT);          /* Unmapped now */

    /* The key is free for a new, empty segment */
    long fresh = shmget(0xB45C, 4096);
    CHECK(fresh >= 0);
    volatile char *empty = shmat(fresh);
    CHECK(empty != NULL && empty[0] == 0);
    CHECK(shmrm(fresh) == 0);
    munmap((void *)empty, 4096);
    CHECK(shmat(-1) == NULL && shmat(9999) == NULL);
}

/* Run `checks` in a child process that has given up root for `uid`; returns
 * the number of checks that failed there. */
static int as_user(unsigned uid, void (*checks)(void)) {
    long pid = fork();
    if (pid == 0) {
        failures = 0;
        if (setuid(uid) != 0) exit(100);
        checks();
        exit(failures);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

static long root_pid;   /* The process running the checks as root */

static void checks_as_alice(void) {
    char buf[16];

    CHECK(getuid() == 1000);
    CHECK(set_time(1893456000UL) == -EPERM);              /* Only root sets the clock */
    struct net_config net;
    CHECK(netconfig(&net, NULL) == 0 && net.ip != 0);   /* Anyone may look at the network settings */
    CHECK(netconfig(NULL, &net) == -EPERM);             /* Only root may change them */
    CHECK(reboot(0) == -EPERM);                         /* Only root may stop the system */
    CHECK(reboot(1) == -EPERM);
    CHECK(setuid(0) == -EPERM);                         /* No way back to root */
    CHECK(setuid(1001) == -EPERM);                      /* Nor sideways */
    CHECK(setuid(1000) == 0);                           /* "Becoming" yourself is allowed */

    /* The system's files are not ours to change, and some not to read */
    CHECK(open("/etc/motd", O_RDONLY) >= 0);
    CHECK(open("/etc/motd", O_WRONLY) == -EACCES);
    CHECK(open("/etc/motd", O_RDONLY | O_TRUNC) >= 0);  /* Truncation needs write access: ignored on a read-only open */
    CHECK(open("/etc/passwd", O_RDONLY) == -EACCES);
    struct stat st;                                     /* Describing it needs no right to read it */
    CHECK(stat("/etc/passwd", &st) == 0 && st.mode == 0600 && st.uid == 0);
    CHECK(truncate("/etc/motd", 0) == -EACCES);         /* Cutting is writing */
    CHECK(utime("/etc/motd", UTIME_NOW) == -EACCES);    /* "Now" needs the right to write ... */
    CHECK(utime("/etc/motd", 1000000000UL) == -EPERM);  /* ... any other time, to own it */
    CHECK(open("/dev/vda", O_RDWR) == -EACCES);
    CHECK(open("/bin/evil", O_WRONLY | O_CREAT) == -EACCES);
    CHECK(open("/disk/evil.txt", O_WRONLY | O_CREAT) == -EACCES);
    CHECK(unlink("/bin/hello") == -EACCES);
    CHECK(unlink("/etc/motd") == -EACCES);
    CHECK(mkdir("/evil") == -EACCES);
    CHECK(chmod("/etc/passwd", 0666) == -EPERM);
    CHECK(chown("/etc/motd", 1000, KEEP_ID) == -EPERM);

    /* Programs can still be run, and devices meant for everyone used */
    long pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/hello", NULL };
        dup2((int)open("/dev/null", O_WRONLY), 1);
        execv("/bin/hello", args);
        exit(1);
    }
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);

    /* Our own file in the shared directory */
    long fd = open("/tmp/alice.txt", O_WRONLY | O_CREAT);
    CHECK(fd >= 0);
    CHECK(write((int)fd, "hers", 4) == 4);
    close((int)fd);
    CHECK(chmod("/tmp/alice.txt", 0600) == 0);          /* The owner may */
    CHECK(utime("/tmp/alice.txt", 1000000000UL) == 0);  /* Her own file: any time she likes */
    CHECK(chown("/tmp/alice.txt", 1001, KEEP_ID) == -EPERM);     /* ... but may not give it away */
    fd = open("/tmp/alice.txt", O_RDONLY);
    CHECK(fd >= 0 && read((int)fd, buf, sizeof(buf)) == 4);
    close((int)fd);

    /* Root's processes are out of reach; our own are not */
    CHECK(kill(root_pid, SIGKILL) == -EPERM);
    pid = fork();
    if (pid == 0) { for (;;) sleep_ms(1000); }
    CHECK(kill(pid, SIGKILL) == 0);
    waitpid(pid, &status, 0);

    /* Low port numbers are reserved for root */
    long sock = socket(SOCK_DGRAM);
    CHECK(bind((int)sock, 80) == -EACCES);
    CHECK(bind((int)sock, 8080) == 0);
    close((int)sock);
}

/* (Run as bob, uid 1001, by check_users) What alice made private stays so */
static void checks_as_bob(void) {
    /* Alice made her file private: Bob can neither read, write nor remove it */
    CHECK(getuid() == 1001);
    CHECK(open("/tmp/alice.txt", O_RDONLY) == -EACCES);
    CHECK(open("/tmp/alice.txt", O_WRONLY) == -EACCES);
    CHECK(unlink("/tmp/alice.txt") == -EACCES);
    CHECK(chmod("/tmp/alice.txt", 0666) == -EPERM);

    /* But he can have files of his own next to it */
    long fd = open("/tmp/bob.txt", O_WRONLY | O_CREAT);
    CHECK(fd >= 0);
    close((int)fd);
    CHECK(unlink("/tmp/bob.txt") == 0);
}

/* --- Groups, the execute bit, set-uid, umask -------------------------------- */

/* The mode of an entry of /tmp, or -1 */
static long tmp_mode(const char *name) {
    struct dirent entry;
    long mode = -1;
    long fd = open("/tmp", O_RDONLY);
    if (fd < 0) return -1;
    while (readdir((int)fd, &entry) == 1) {
        if (strcmp(entry.name, name) == 0) mode = (long)entry.mode;
    }
    close((int)fd);
    return mode;
}

/* Copy one file to another (created or truncated). 0, or -1 */
static int copy_file(const char *from, const char *to) {
    static char buf[4096];
    long in = open(from, O_RDONLY);
    long out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    long n = -1;
    if (in >= 0 && out >= 0) {
        while ((n = read((int)in, buf, sizeof(buf))) > 0) {
            if (write((int)out, buf, (size_t)n) != n) { n = -1; break; }
        }
    }
    if (in >= 0) close((int)in);
    if (out >= 0) close((int)out);
    return n == 0 ? 0 : -1;
}

/* Run a program and collect what it prints; returns its exit status */
static int run_capture(const char *path, char *out, size_t size) {
    int p[2];
    if (pipe(p) != 0) return -1;
    long pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char *args[] = { (char *)path, NULL };
        exit(execve(path, args, NULL) == -EACCES ? 126 : 127);
    }
    close(p[1]);
    size_t used = 0;
    long n;
    while (used < size - 1 && (n = read(p[0], out + used, size - 1 - used)) > 0) used += (size_t)n;
    out[used] = '\0';
    close(p[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

/* Whether `what` occurs in `text` */
static int contains(const char *text, const char *what) {
    size_t len = strlen(what);
    for (; *text; text++) if (strncmp(text, what, len) == 0) return 1;
    return 0;
}

static long shared_key_id;      /* A segment root made, for the others to be refused */

/* Search permission, as alice: a directory closed to everyone else, with
 * an ordinary readable file and a program in it */
static void closed_directory_as_alice(void) {
    CHECK(mkdir("/tmp/closed") == 0);
    long fd = open("/tmp/closed/open.txt", O_WRONLY | O_CREAT);
    CHECK(fd >= 0 && write((int)fd, "inside", 6) == 6);
    close((int)fd);
    CHECK(copy_file("/bin/false", "/tmp/closed/false") == 0 && chmod("/tmp/closed/false", 0755) == 0);
    CHECK(chmod("/tmp/closed", 0700) == 0);
    fd = open("/tmp/closed/open.txt", O_RDONLY);        /* The owner still gets in */
    CHECK(fd >= 0);
    close((int)fd);
}

/* Search permission, as bob: a readable file in a directory he may not
 * search is out of reach by every path to it; with search (but not read)
 * permission he can reach a name he knows, but not list the directory */
static void closed_directory_as_bob(void) {
    CHECK(open("/tmp/closed/open.txt", O_RDONLY) == -EACCES);
    CHECK(open("/tmp/closed/../../etc/motd", O_RDONLY) == -EACCES);   /* Not a way through either */
    CHECK(open("/tmp/closed/new.txt", O_WRONLY | O_CREAT) == -EACCES);
    CHECK(mkdir("/tmp/closed/dir") == -EACCES);
    CHECK(chdir("/tmp/closed") == -EACCES);
    CHECK(chmod("/tmp/closed/open.txt", 0777) == -EACCES);
    struct stat st;
    CHECK(stat("/tmp/closed/open.txt", &st) == -EACCES);           /* Nor even described */
    CHECK(symlink("/tmp/closed/open.txt", "/tmp/bob-link") == 0);     /* A link is no way round */
    CHECK(open("/tmp/bob-link", O_RDONLY) == -EACCES);
    CHECK(unlink("/tmp/bob-link") == 0);
    long pid = fork();
    if (pid == 0) {
        char *args[] = { "/tmp/closed/false", NULL };          /* (Run, it would exit with 1) */
        exit(execv("/tmp/closed/false", args) == -EACCES ? 0 : 2);
    }
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
}

/* As alice again: 0711, search for everyone but no listing */
static void closed_directory_opened_as_alice(void) {
    CHECK(chmod("/tmp/closed", 0711) == 0);
}

/* As bob: a known name works now, the listing still does not */
static void closed_directory_known_name_as_bob(void) {
    long fd = open("/tmp/closed/open.txt", O_RDONLY);
    char buf[8];
    CHECK(fd >= 0 && read((int)fd, buf, sizeof(buf)) == 6);
    close((int)fd);
    CHECK(open("/tmp/closed", O_RDONLY) == -EACCES);
    CHECK(chdir("/tmp/closed") == 0 && chdir("/") == 0);
}

/* As alice, in group 100 */
static void permission_checks_as_alice(void) {
    static char out[256];
    CHECK(getuid() == 1000 && geteuid() == 1000 && getgid() == 100);
    CHECK(setgid(0) == -EPERM && setgid(100) == 0);

    /* The group's digit applies to us: read, not write */
    long fd = open("/tmp/group.txt", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    CHECK(open("/tmp/group.txt", O_WRONLY) == -EACCES);

    /* A set-uid program runs with its owner's rights and knows who we are */
    CHECK(run_capture("/tmp/suid-id", out, sizeof(out)) == 0);
    CHECK(contains(out, "uid 1000\n") && contains(out, "effective uid 0 (root)"));
    CHECK(run_capture("/bin/id", out, sizeof(out)) == 0);
    CHECK(contains(out, "uid 1000\n") && !contains(out, "effective"));
    CHECK(getuid() == 1000 && geteuid() == 1000);            /* Nothing rubbed off on us */

    /* We may not make our own: the bit can be set only on our own files,
     * and then it lends out our rights, not root's */
    CHECK(chmod("/tmp/suid-id", 04777) == -EPERM);
    CHECK(open("/tmp/suid-id", O_WRONLY) == -EACCES);
    CHECK(copy_file("/bin/id", "/tmp/alice-id") == 0);
    CHECK(tmp_mode("alice-id") == 0644);
    CHECK(run_capture("/tmp/alice-id", out, sizeof(out)) == 126);   /* Not marked executable: refused */
    CHECK(chmod("/tmp/alice-id", 04755) == 0 && tmp_mode("alice-id") == 04755);
    CHECK(run_capture("/tmp/alice-id", out, sizeof(out)) == 0 && !contains(out, "effective"));
    fd = open("/tmp/alice-id", O_WRONLY | O_APPEND);          /* Writing to it takes the bit away */
    CHECK(fd >= 0 && tmp_mode("alice-id") == 0755);
    if (fd >= 0) close((int)fd);

    /* Group and owner of our files: our own group, yes; anything else, no */
    CHECK(chown("/tmp/alice-id", KEEP_ID, 100) == 0);
    CHECK(chown("/tmp/alice-id", KEEP_ID, 0) == -EPERM);
    CHECK(chown("/tmp/alice-id", 0, KEEP_ID) == -EPERM);
    CHECK(unlink("/tmp/alice-id") == 0);

    /* umask is ours to choose and shapes what we create */
    CHECK(umask(077) == 022);
    fd = open("/tmp/private.txt", O_WRONLY | O_CREAT);
    CHECK(fd >= 0 && close((int)fd) == 0 && tmp_mode("private.txt") == 0600);
    CHECK(mkdir("/tmp/private-dir") == 0 && tmp_mode("private-dir") == 0700);
    CHECK(umask(022) == 077);
    CHECK(unlink("/tmp/private.txt") == 0 && unlink("/tmp/private-dir") == 0);

    /* Shared memory belongs to whoever made it */
    CHECK(shmget(0x5EC0DE, 4096) == -EACCES);                 /* Root's: not found by its key */
    CHECK(shmat(shared_key_id) == NULL);
    CHECK(shmrm(shared_key_id) == -EACCES);
    long mine = shmget(0xA11CE, 4096);
    CHECK(mine >= 0);
    char *mapped = shmat(mine);
    CHECK(mapped != NULL);
    if (mapped) mapped[0] = 'a';
    CHECK(shmrm(mine) == 0);
}

/* As bob, in a group that owns nothing */
static void permission_checks_as_bob(void) {
    static char out[64];
    CHECK(getuid() == 1001 && getgid() == 200);
    CHECK(open("/tmp/group.txt", O_RDONLY) == -EACCES);       /* Not the owner, not in the group */
    CHECK(run_capture("/tmp/root-only", out, sizeof(out)) == 126);
}

/* Run `checks` in a child that has become user `uid` in group `gid`.
 * Returns its number of failures (100 if it could not change user). */
static int as_user_in_group(unsigned uid, unsigned gid, void (*checks)(void)) {
    long pid = fork();
    if (pid == 0) {
        failures = 0;
        if (setgid(gid) != 0 || setuid(uid) != 0) exit(100);
        checks();
        exit(failures);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

/* umask, groups, chown of the group, and set-uid programs */
static void check_groups_and_setuid(void) {
    static char out[128];
    CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0);

    /* The mask a new file's mode is cut down by */
    CHECK(umask(022) == 022);
    long fd = open("/tmp/group.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "g", 1) == 1 && close((int)fd) == 0);
    CHECK(tmp_mode("group.txt") == 0644);
    CHECK(chmod("/tmp/group.txt", 0640) == 0 && chown("/tmp/group.txt", KEEP_ID, 100) == 0);
    CHECK(chmod("/tmp/group.txt", 010000) == -EINVAL);        /* No such bit */

    /* A copy of a program is not a program until it is marked as one */
    CHECK(copy_file("/bin/id", "/tmp/suid-id") == 0);
    CHECK(run_capture("/tmp/suid-id", out, sizeof(out)) == 126);
    CHECK(chmod("/tmp/suid-id", 04755) == 0);
    CHECK(run_capture("/tmp/suid-id", out, sizeof(out)) == 0 && contains(out, "uid 0 (root)"));
    CHECK(copy_file("/bin/hello", "/tmp/root-only") == 0 && chmod("/tmp/root-only", 0700) == 0);

    shared_key_id = shmget(0x5EC0DE, 4096);
    CHECK(shared_key_id >= 0);

    CHECK(as_user_in_group(1000, 100, permission_checks_as_alice) == 0);
    CHECK(as_user_in_group(1001, 200, permission_checks_as_bob) == 0);

    /* Root giving a set-uid file away does not hand out a set-uid program */
    CHECK(chown("/tmp/suid-id", 1001, KEEP_ID) == 0 && tmp_mode("suid-id") == 0755);

    CHECK(shmrm(shared_key_id) == 0);
    CHECK(unlink("/tmp/group.txt") == 0 && unlink("/tmp/suid-id") == 0 && unlink("/tmp/root-only") == 0);
}

/* As bob (1001, group 100) with supplementary group 50: a file of group 50
 * is readable through it, and bob may move a file of his into group 50 but
 * not into 51; he may not change his groups */
static void group_checks_as_bob(void) {
    unsigned groups[4];
    CHECK(getgroups(4, groups) == 1 && groups[0] == 50);
    CHECK(setgroups(0, NULL) == -EPERM);                      /* Membership is root's to give */
    long fd = open("/tmp/staff.txt", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    CHECK(open("/tmp/staff.txt", O_WRONLY) == -EACCES);       /* The group's digit: r-- */
    fd = open("/tmp/bobs.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    CHECK(chown("/tmp/bobs.txt", KEEP_ID, 50) == 0);
    CHECK(chown("/tmp/bobs.txt", KEEP_ID, 51) == -EPERM);
}

/* The same bob without the supplementary group: the others' digit (---) */
static void group_checks_without_groups(void) {
    CHECK(getgroups(0, NULL) == 0);
    CHECK(open("/tmp/staff.txt", O_RDONLY) == -EACCES);
}

/* setgroups and getgroups: root only, at most 16, copied by fork, kept by
 * execve, and counted by the permission check */
static void check_supplementary_groups(void) {
    static char out[128];
    unsigned groups[17] = { 50, 51, 52 }, got[16];
    CHECK(getgroups(0, NULL) == 0);
    CHECK(setgroups(17, groups) == -EINVAL);                  /* More than 16 */
    CHECK(setgroups(2, (const unsigned *)16) == -EFAULT);
    CHECK(getgroups(0, NULL) == 0);                           /* Neither changed anything */
    CHECK(setgroups(3, groups) == 0);
    CHECK(getgroups(0, NULL) == 3 && getgroups(2, got) == -EINVAL);
    CHECK(getgroups(16, got) == 3 && got[0] == 50 && got[1] == 51 && got[2] == 52);

    long pid = fork();
    if (pid == 0) exit(getgroups(16, got) == 3 && got[2] == 52 ? 0 : 1);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);   /* Copied by fork */
    CHECK(run_capture("/bin/id", out, sizeof(out)) == 0 && contains(out, "groups 50 51 52\n"));  /* Kept by execve */

    long fd = open("/tmp/staff.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && close((int)fd) == 0);
    CHECK(chmod("/tmp/staff.txt", 0640) == 0 && chown("/tmp/staff.txt", 1000, 50) == 0);
    CHECK(setgroups(1, groups) == 0);
    CHECK(as_user_in_group(1001, 100, group_checks_as_bob) == 0);
    CHECK(setgroups(0, NULL) == 0);
    CHECK(as_user_in_group(1001, 100, group_checks_without_groups) == 0);
    CHECK(unlink("/tmp/staff.txt") == 0 && unlink("/tmp/bobs.txt") == 0);
}

/* As bob (1001, group 200) in /tmp, which is sticky: alice's file there
 * cannot be removed or renamed away by him, nor replaced by a rename; his
 * own can. In /tmp/open, writable by all and not sticky, hers can. */
static void sticky_checks_as_bob(void) {
    CHECK(unlink("/tmp/st-alice") == -EACCES);
    CHECK(rename("/tmp/st-alice", "/tmp/st-moved") == -EACCES);
    long fd = open("/tmp/st-bob", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    CHECK(rename("/tmp/st-bob", "/tmp/st-alice") == -EEXIST);     /* Nothing is replaced */
    CHECK(rename("/tmp/st-bob", "/tmp/st-bob2") == 0);
    CHECK(unlink("/tmp/st-bob2") == 0);
    CHECK(unlink("/tmp/open/alice") == 0);                      /* Not sticky: writing there is enough */
}

/* As bob (1001, group 200): a set-gid program of group 50 runs with 50 as
 * its effective group and 200 as its real one; bob may go back to 200 but
 * not take 50; his own file may be made set-gid only for a group he is in,
 * and writing to it takes the bit away */
static void setgid_checks_as_bob(void) {
    static char out[128];
    CHECK(getgid() == 200 && getegid() == 200);
    CHECK(run_capture("/tmp/sgid-id", out, sizeof(out)) == 0 && contains(out, "gid 200\n") &&
          contains(out, "effective gid 50\n"));
    CHECK(setgid(50) == -EPERM && setgid(200) == 0);
    long fd = open("/tmp/bob-tool", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    CHECK(chmod("/tmp/bob-tool", 02755) == 0 && tmp_mode("bob-tool") == 02755);    /* His own group */
    CHECK(chown("/tmp/bob-tool", KEEP_ID, 50) == -EPERM);
    fd = open("/tmp/bob-tool", O_WRONLY);
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    CHECK(tmp_mode("bob-tool") == 0755);                          /* Rewritten by him: no longer set-gid */
    CHECK(chmod("/tmp/sgid-id", 02755) == -EPERM);                /* Not his */
    CHECK(unlink("/tmp/bob-tool") == 0);
}

/* As alice (1000, group 100) without group 50: chmod of her file in group
 * 50 with the set-gid bit keeps the rest and quietly leaves the bit off */
static void setgid_checks_as_alice(void) {
    CHECK(chmod("/tmp/st-alice", 02750) == 0 && tmp_mode("st-alice") == 0750);
}

static int sh_capture(const char *command, char *out, size_t size);   /* (Defined with the shell's checks) */

/* The sticky bit (/tmp has it) and set-gid programs */
static void check_sticky_and_setgid(void) {
    static char out[512];
    struct stat st;
    CHECK(stat("/tmp", &st) == 0 && st.mode == 01777);
    CHECK(sh_capture("ls -l / | grep ' tmp'", out, sizeof(out)) == 0 && strncmp(out, "drwxrwxrwt", 10) == 0);

    long fd = open("/tmp/st-alice", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && close((int)fd) == 0);
    CHECK(chown("/tmp/st-alice", 1000, 100) == 0);
    CHECK(mkdir("/tmp/open") == 0 && chmod("/tmp/open", 0777) == 0);
    fd = open("/tmp/open/alice", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && close((int)fd) == 0);
    CHECK(chown("/tmp/open/alice", 1000, 100) == 0);
    CHECK(as_user_in_group(1001, 200, sticky_checks_as_bob) == 0);
    CHECK(tmp_mode("st-alice") == 0644);                          /* Still there, untouched */

    /* Root's chown of the group comes first: it would take the bit away */
    CHECK(copy_file("/bin/id", "/tmp/sgid-id") == 0);
    CHECK(chown("/tmp/sgid-id", KEEP_ID, 50) == 0 && chmod("/tmp/sgid-id", 02755) == 0);
    CHECK(tmp_mode("sgid-id") == 02755);
    CHECK(sh_capture("ls -l /tmp | grep sgid-id", out, sizeof(out)) == 0 && strncmp(out, "-rwxr-sr-x", 10) == 0);
    CHECK(as_user_in_group(1001, 200, setgid_checks_as_bob) == 0);
    CHECK(chown("/tmp/st-alice", KEEP_ID, 50) == 0);
    CHECK(as_user_in_group(1000, 100, setgid_checks_as_alice) == 0);
    CHECK(chown("/tmp/sgid-id", KEEP_ID, 51) == 0 && tmp_mode("sgid-id") == 0755);   /* A new group did not ask for it */

    CHECK(unlink("/tmp/st-alice") == 0 && unlink("/tmp/sgid-id") == 0);
    CHECK(unlink("/tmp/open") == 0);
}

/* Permission bits as alice and bob, and root's freedom from them */
static void check_users(void) {
    CHECK(getuid() == 0);                               /* The checks themselves run as root */
    root_pid = getpid();

    CHECK(as_user(1000, checks_as_alice) == 0);
    CHECK(as_user(1001, checks_as_bob) == 0);
    CHECK(as_user(1000, closed_directory_as_alice) == 0);
    CHECK(as_user(1001, closed_directory_as_bob) == 0);
    CHECK(as_user(1000, closed_directory_opened_as_alice) == 0);
    CHECK(as_user(1001, closed_directory_known_name_as_bob) == 0);
    CHECK(unlink("/tmp/closed/open.txt") == 0 && unlink("/tmp/closed/false") == 0 && unlink("/tmp/closed") == 0);

    /* Still root here: the children's change of user did not touch us */
    CHECK(getuid() == 0);

    /* Root is not bound by the bits, and may reassign files */
    long fd = open("/tmp/alice.txt", O_RDONLY);
    CHECK(fd >= 0);
    close((int)fd);
    CHECK(chown("/tmp/alice.txt", 1001, KEEP_ID) == 0);
    CHECK(chown("/tmp/no-such-file", 1001, KEEP_ID) == -ENOENT);
    CHECK(chmod((const char *)0xffffffff80000000, 0600) == -EFAULT);
    CHECK(unlink("/tmp/alice.txt") == 0);
}

/* Write `len` bytes as the whole of `path` (root: the checks run as root) */
static long replace_file(const char *path, const char *text, size_t len) {
    long fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return fd;
    long written = write((int)fd, text, len);
    close((int)fd);
    return written;
}

/* The account file is read by a library that holds 4 KiB of it. A larger
 * file must not be cut short by a change of password (every account after
 * the first 4 KiB would go), and a malformed number must not read as 0
 * (uid 0 is root). The file is put back afterwards. */
static void check_password_file(void) {
    static char original[4096], big[8192];
    long fd = open(PASSWORD_FILE, O_RDONLY);
    CHECK(fd >= 0);
    long original_len = read((int)fd, original, sizeof(original));
    close((int)fd);
    CHECK(original_len > 0 && original_len < (long)sizeof(original));
    if (original_len <= 0) return;

    /* 40 more accounts: about 5.5 KiB in all */
    size_t len = (size_t)original_len;
    memcpy(big, original, len);
    for (int i = 0; i < 40; i++) {
        char line[160];
        size_t n = 0;
        const char *head = "filler";
        while (*head) line[n++] = *head++;
        line[n++] = (char)('0' + i / 10);
        line[n++] = (char)('0' + i % 10);
        const char *rest = ":2000:100:4096:00112233445566778899aabbccddeeff:"
                           "0011223344556677889900112233445566778899001122334455667788990011\n";
        while (*rest) line[n++] = *rest++;
        memcpy(big + len, line, n);
        len += n;
    }
    CHECK(replace_file(PASSWORD_FILE, big, len) == (long)len);
    CHECK(account_set_password("alice", "changed") == -EFBIG);
    fd = open(PASSWORD_FILE, O_RDONLY);
    static char after[8192];
    long after_len = read((int)fd, after, sizeof(after));
    close((int)fd);
    CHECK(after_len == (long)len && memcmp(after, big, len) == 0);

    /* "x" is not a user id */
    static const char evil[] = "evil:x:0:4096:00112233445566778899aabbccddeeff:"
                               "0011223344556677889900112233445566778899001122334455667788990011\n";
    CHECK(replace_file(PASSWORD_FILE, evil, sizeof(evil) - 1) == (long)sizeof(evil) - 1);
    struct account account;
    CHECK(account_by_name("evil", &account) == -ENOENT);

    CHECK(replace_file(PASSWORD_FILE, original, (size_t)original_len) == original_len);
    CHECK(account_by_name("alice", &account) == 0 && account.uid == 1000);
}

/* The time of day, and the library's dates: printed and parsed, with the
 * calendar's corners (a leap day, a day that does not exist) */
static void check_time_of_day(void) {
    unsigned long now = time_ns() / 1000000000UL;
    CHECK(now > 1735689600UL && now < 4291747199UL);      /* Between 2025 and 2106 */
    unsigned long later = time_ns() / 1000000000UL;
    CHECK(later >= now && later - now <= 1);

    char text[20];
    format_date(951827696UL, text);
    CHECK(strcmp(text, "2000-02-29 12:34:56") == 0);
    unsigned long seconds = 0;
    CHECK(parse_date("2000-02-29 12:34:56", &seconds) == 0 && seconds == 951827696UL);
    CHECK(parse_date("2026-10-20 00:00", &seconds) == 0 && seconds == 1792454400UL);
    CHECK(parse_date("2001-02-29 00:00", &seconds) == -EINVAL);   /* Not a leap year */
    CHECK(parse_date("2026-13-01 00:00", &seconds) == -EINVAL);
    CHECK(parse_date("2026-10-20 24:00", &seconds) == -EINVAL);
    CHECK(parse_date("2026-10-20", &seconds) == -EINVAL);
    CHECK(parse_date("2026-10-20 10:00x", &seconds) == -EINVAL);

    /* Root may set it (to 2030, briefly) and the change is seen at once;
     * out of range is refused. Afterwards it is put back exactly, to the
     * nanosecond (the kernel's own tests, run later, compare the time with
     * the CMOS clock: put back to the second, it was sometimes 2 s out) */
    unsigned long saved_ns = time_ns(), saved_at = clock_ns();
    CHECK(set_time(100UL) == -EINVAL);
    CHECK(set_time(1893456000UL) == 0);
    CHECK(time_ns() / 1000000000UL - 1893456000UL <= 1);
    CHECK(set_time_ns(saved_ns + (clock_ns() - saved_at)) == 0);
    unsigned long back = time_ns();
    CHECK(back >= saved_ns && back - saved_ns < 1000000000UL);
}

/* poll: a child writes into the second of two pipes after a while; the
 * parent, polling both, wakes with that one marked. And the errors. */
static void check_poll(void) {
    int a[2], b[2];
    CHECK(pipe(a) == 0 && pipe(b) == 0);
    struct pollfd fds[2] = { { a[0], POLLIN, 0 }, { b[0], POLLIN, 0 } };
    CHECK(poll(fds, 2, 0) == 0);                          /* Nothing yet: just look */
    CHECK(poll(fds, 2, 50) == 0);                         /* ...or wait 50 ms for nothing */

    long pid = fork();
    if (pid == 0) {
        sleep_ms(50);
        write(b[1], "!", 1);
        exit(0);
    }
    unsigned long start = clock_ns();
    CHECK(poll(fds, 2, 5000) == 1);
    CHECK(fds[0].revents == 0 && fds[1].revents == POLLIN);
    CHECK(clock_ns() - start < 2000000000UL);             /* Woken by the write, not the limit */
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);

    /* The child's end of b is closed with it: b's reader sees the end too */
    close(b[1]);
    char c;
    CHECK(read(b[0], &c, 1) == 1);
    CHECK(poll(&fds[1], 1, 0) == 1 && fds[1].revents == (POLLIN | POLLHUP));

    struct pollfd bad = { 99, POLLIN, 0 };
    CHECK(poll(&bad, 1, 0) == 1 && bad.revents == POLLNVAL);
    CHECK(poll((struct pollfd *)0xffffffff80000000, 1, 0) == -EFAULT);
    CHECK(poll(fds, 65, 0) == -EINVAL);                   /* More than the kernel takes at once */
    close(a[0]); close(a[1]); close(b[0]);
}

/* Run a program with up to three arguments, its output thrown away; its
 * exit status, or -1 */
static int run_quietly(const char *path, const char *a1, const char *a2, const char *a3) {
    long pid = fork();
    if (pid == 0) {
        char *args[] = { (char *)path, (char *)a1, (char *)a2, (char *)a3, NULL };
        dup2((int)open("/dev/null", O_WRONLY), 1);
        dup2((int)open("/dev/null", O_WRONLY), 2);
        execv(path, args);
        exit(127);
    }
    int status = -1;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) return -1;
    return status;
}

/* A running program is read from its file page by page, so the file must
 * stay as it is while it runs: not opened for writing or truncated
 * (-ETXTBSY), not removed (-EBUSY); and a file open for writing cannot be
 * run. All on a copy, so a failure harms nothing that matters. */
static void check_running_program_file(void) {
    CHECK(run_quietly("/bin/cp", "/bin/sleep", "/tmp/running", NULL) == 0);
    long pid = fork();
    if (pid == 0) {
        char *args[] = { "/tmp/running", "30", NULL };
        execv("/tmp/running", args);
        exit(127);
    }
    CHECK(pid > 0);
    CHECK(wait_in_syscall(pid, SYS_SLEEP));                /* Running (exec done): asleep in its sleep */
    CHECK(open("/tmp/running", O_WRONLY) == -ETXTBSY);
    CHECK(open("/tmp/running", O_RDWR | O_APPEND) == -ETXTBSY);
    CHECK(truncate("/tmp/running", 0) == -ETXTBSY);
    CHECK(unlink("/tmp/running") == -EBUSY);
    long fd = open("/tmp/running", O_RDONLY);              /* Reading it is fine */
    CHECK(fd >= 0);
    if (fd >= 0) close((int)fd);
    int status = -1;
    CHECK(kill(pid, SIGKILL) == 0 && waitpid(pid, &status, 0) == pid && status == 128 + SIGKILL);

    /* Not running any more: it may be written; while it is open for that, run it may not */
    fd = open("/tmp/running", O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(run_quietly("/tmp/running", "0", NULL, NULL) == 127);
    if (fd >= 0) close((int)fd);
    CHECK(run_quietly("/tmp/running", "0", NULL, NULL) == 0);
    CHECK(unlink("/tmp/running") == 0);
}

/* /bin/dhcp's lease arithmetic (its own checks, -T: when to renew, rebind
 * and give up, from what a server may send), and options it must refuse
 * before touching the network */
static void check_dhcp_lease_times(void) {
    CHECK(run_quietly("/bin/dhcp", "-T", NULL, NULL) == 0);
    CHECK(run_quietly("/bin/dhcp", "-t", "5", NULL) == 2);      /* Only with -d */
    CHECK(run_quietly("/bin/dhcp", "-t", "x", NULL) == 2);
    CHECK(run_quietly("/bin/dhcp", "-d", "-n", "0") == 2);
}

/* Whether two files hold the same bytes */
static int same_contents(const char *a, const char *b) {
    static char x[4096], y[4096];
    long fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    int same = fa >= 0 && fb >= 0;
    while (same) {
        long n = read((int)fa, x, sizeof(x)), m = read((int)fb, y, sizeof(y));
        if (n != m || n < 0 || memcmp(x, y, (size_t)n) != 0) same = 0;
        if (n <= 0) break;
    }
    if (fa >= 0) close((int)fa);
    if (fb >= 0) close((int)fb);
    return same;
}

/* Links: symbolic ones in memory and on the disk, hard ones on the disk
 * only; reading a link's target; the errors */
static void check_links(void) {
    char buf[64];
    long fd = open("/tmp/link-target", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "linked", 6) == 6);
    close((int)fd);

    CHECK(symlink("/tmp/link-target", "/tmp/sym") == 0);
    CHECK(symlink("link-target", "/tmp/rel") == 0);
    CHECK(readlink("/tmp/sym", buf, sizeof(buf)) == 16 && memcmp(buf, "/tmp/link-target", 16) == 0);
    CHECK(readlink("/tmp/sym", buf, 4) == 4);                     /* Cut to the buffer */
    CHECK(readlink("/tmp/link-target", buf, sizeof(buf)) == -EINVAL);
    fd = open("/tmp/rel", O_RDONLY);
    CHECK(fd >= 0 && read((int)fd, buf, sizeof(buf)) == 6);
    close((int)fd);
    CHECK(symlink("x", "/tmp/sym") == -EEXIST);
    CHECK(symlink("/tmp/loop-b", "/tmp/loop-a") == 0 && symlink("/tmp/loop-a", "/tmp/loop-b") == 0);
    CHECK(open("/tmp/loop-a", O_RDONLY) == -ELOOP);
    CHECK(link("/tmp/link-target", "/tmp/hard") == -EPERM);       /* Not in memory */
    CHECK(link("/bin", "/tmp/bin-again") == -EPERM);              /* Nor of directories */

    /* The rest needs the system disk as the root (the rescue system has
     * none: its /root is in memory too) */
    if (!root_on_disk()) goto clean_up;

    CHECK(link("/tmp/link-target", "/root/hard") == -EXDEV);      /* Not across filesystems */
    /* On the disk: a hard link is the same file */
    fd = open("/root/one", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "same", 4) == 4);
    close((int)fd);
    CHECK(run_quietly("/bin/cp", "/bin/syscheck", "/root/copy", NULL) == 0);   /* cp onto the disk */
    CHECK(same_contents("/bin/syscheck", "/root/copy") && unlink("/root/copy") == 0);
    CHECK(link("/root/one", "/root/two") == 0);
    struct stat one, two;                                         /* One inode, two names */
    CHECK(stat("/root/one", &one) == 0 && stat("/root/two", &two) == 0);
    CHECK(one.ino != 0 && one.ino == two.ino && one.links == 2 && two.size == 4);
    CHECK(unlink("/root/one") == 0);
    CHECK(stat("/root/two", &two) == 0 && two.links == 1);
    fd = open("/root/two", O_RDONLY);
    CHECK(fd >= 0 && read((int)fd, buf, sizeof(buf)) == 4 && memcmp(buf, "same", 4) == 0);
    close((int)fd);
    CHECK(unlink("/root/two") == 0);

clean_up:;
    static const char *const gone[] = { "/tmp/sym", "/tmp/rel", "/tmp/loop-a", "/tmp/loop-b", "/tmp/link-target" };
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) CHECK(unlink(gone[i]) == 0);
}

/* stat, lstat and fstat: files, links, directories, and open files that
 * are not files at all */
static void check_stat(void) {
    struct stat st, other;
    static char text[64];
    long fd = open("/etc/motd", O_RDONLY);
    long n = fd >= 0 ? read((int)fd, text, sizeof(text)) : -1;
    CHECK(stat("/etc/motd", &st) == 0);
    CHECK(st.type == S_FILE && (long)st.size == n && st.mode == 0644 && st.uid == 0 && st.links == 1);
    CHECK(fd >= 0 && fstat((int)fd, &other) == 0 && other.size == st.size && other.ino == st.ino);
    close((int)fd);

    CHECK(stat("/bin", &st) == 0 && st.type == S_DIR && st.links >= 2 && (st.mode & 0111) != 0);

    /* A link is described itself by lstat, and as what it names by stat */
    fd = open("/tmp/stat-target", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "twelve bytes", 12) == 12);
    close((int)fd);
    CHECK(symlink("stat-target", "/tmp/stat-link") == 0);
    CHECK(lstat("/tmp/stat-link", &st) == 0 && st.type == S_LINK && st.size == 11);
    CHECK(stat("/tmp/stat-link", &st) == 0 && st.type == S_FILE && st.size == 12);
    CHECK(stat("/tmp/stat-target", &other) == 0 && other.ino == st.ino && other.dev == st.dev && st.dev == 1); /* The same file */
    CHECK(stat("/tmp", &other) == 0 && other.ino != st.ino);
    CHECK(unlink("/tmp/stat-link") == 0 && unlink("/tmp/stat-target") == 0);

    /* Open files that are not files */
    int p[2];
    CHECK(pipe(p) == 0 && fstat(p[0], &st) == 0 && st.type == S_PIPE && st.size == 0 && st.dev == 0);
    close(p[0]); close(p[1]);
    fd = open("/dev/console", O_RDONLY);
    CHECK(fd >= 0 && fstat((int)fd, &st) == 0 && st.type == S_TERMINAL);
    close((int)fd);
    fd = socket(SOCK_DGRAM);
    CHECK(fd >= 0 && fstat((int)fd, &st) == 0 && st.type == S_SOCKET);
    close((int)fd);

    /* What is not there, and pointers that are not ours */
    CHECK(stat("/no/such", &st) == -ENOENT);
    CHECK(stat("/etc/motd/x", &st) == -ENOTDIR);
    CHECK(fstat(99, &st) == -EBADF);
    CHECK(stat("/etc/motd", (struct stat *)0xffffffff80000000) == -EFAULT);
    CHECK(stat((const char *)0xffffffff80000000, &st) == -EFAULT);
    CHECK(fstat(1, (struct stat *)8) == -EFAULT);
}

/* truncate, ftruncate and utime: lengths both ways, what they refuse */
static void check_truncate(void) {
    struct stat st;
    char buf[16];
    long fd = open("/tmp/len", O_RDWR | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write((int)fd, "0123456789", 10) == 10);
    CHECK(ftruncate((int)fd, 4) == 0 && fstat((int)fd, &st) == 0 && st.size == 4);
    CHECK(truncate("/tmp/len", 12) == 0 && stat("/tmp/len", &st) == 0 && st.size == 12);
    CHECK(lseek((int)fd, 0, SEEK_SET) == 0 && read((int)fd, buf, sizeof(buf)) == 12);
    CHECK(memcmp(buf, "0123\0\0\0\0\0\0\0\0", 12) == 0);              /* Cut data does not come back */
    close((int)fd);
    fd = open("/tmp/len", O_RDONLY);
    CHECK(ftruncate((int)fd, 0) == -EINVAL);                            /* Not open for writing */
    close((int)fd);
    int p[2];
    CHECK(pipe(p) == 0 && ftruncate(p[1], 0) == -EINVAL);
    close(p[0]); close(p[1]);
    CHECK(ftruncate(99, 0) == -EBADF);
    fd = open("/dev/null", O_WRONLY);                                  /* A device has no length */
    CHECK(fd >= 0 && ftruncate((int)fd, 100) == -EINVAL && truncate("/dev/zero", 0) == -EINVAL);
    close((int)fd);
    CHECK(truncate("/tmp", 0) == -EISDIR && truncate("/no/such", 0) == -ENOENT);
    CHECK(truncate("/tmp/len", 0x100000000UL) == -EFBIG);
    CHECK(truncate((const char *)0xffffffff80000000, 0) == -EFAULT);

    CHECK(utime("/tmp/len", 1000000000UL) == 0 && stat("/tmp/len", &st) == 0 && st.mtime == 1000000000U);
    CHECK(utime("/tmp/len", UTIME_NOW) == 0 && stat("/tmp/len", &st) == 0 && st.mtime > 1700000000U);
    CHECK(utime("/tmp/len", 100UL) == -EINVAL);                         /* Before 1980 */
    CHECK(utime("/no/such", UTIME_NOW) == -ENOENT);
    CHECK(unlink("/tmp/len") == 0);
}

/* The line reader the text tools use, and cp: a line longer than its
 * buffer, a last line without '\n', empty lines; a large file copied
 * exactly, its mode kept, into a directory, never onto itself */
static void check_text_tools(void) {
    /* (A file, not a pipe: this is more than a pipe holds, and nobody
     * would be reading it yet) */
    long fd = open("/tmp/lines", O_RDWR | O_CREAT | O_TRUNC);
    static char longline[6000];
    memset(longline, 'x', sizeof(longline));
    CHECK(fd >= 0 && write((int)fd, longline, sizeof(longline)) == (long)sizeof(longline));
    CHECK(write((int)fd, "\n\nlast", 6) == 6);
    CHECK(lseek((int)fd, 0, SEEK_SET) == 0);
    static lines_t in;
    lines_open(&in, (int)fd);
    CHECK(lines_next(&in) == 6000 && in.line[5999] == 'x' && in.line[6000] == '\0');
    CHECK(lines_next(&in) == 0);                                    /* An empty line */
    CHECK(lines_next(&in) == 4 && strcmp(in.line, "last") == 0);  /* No '\n' at the end */
    CHECK(lines_next(&in) == -1 && lines_next(&in) == -1);
    lines_close(&in);
    close((int)fd);
    CHECK(unlink("/tmp/lines") == 0);

    struct stat a, b;
    CHECK(run_quietly("/bin/cp", "/bin/syscheck", "/tmp/copy", NULL) == 0);
    CHECK(same_contents("/bin/syscheck", "/tmp/copy"));
    CHECK(stat("/bin/syscheck", &a) == 0 && stat("/tmp/copy", &b) == 0 && b.mode == a.mode && b.size == a.size);
    CHECK(mkdir("/tmp/into") == 0 && run_quietly("/bin/cp", "/tmp/copy", "/tmp/into", NULL) == 0);
    CHECK(same_contents("/bin/syscheck", "/tmp/into/copy"));
    CHECK(run_quietly("/bin/cp", "/tmp/copy", "/tmp/into/../copy", NULL) == 1);   /* Itself, by another path */
    CHECK(stat("/tmp/copy", &b) == 0 && b.size == a.size);                           /* ... and not emptied */
    CHECK(run_quietly("/bin/cp", "/tmp/into", "/tmp/x", NULL) == 1);                 /* A directory */
    CHECK(run_quietly("/bin/cp", "/no/such", "/tmp/x", NULL) == 1);
    CHECK(unlink("/tmp/into/copy") == 0 && unlink("/tmp/into") == 0 && unlink("/tmp/copy") == 0);
}

/* Run `sh -c command` and collect what it prints (stdout); its exit status */
static int sh_capture(const char *command, char *out, size_t size) {
    int p[2];
    if (pipe(p) != 0) return -1;
    long pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        int null = (int)open("/dev/null", O_RDWR);
        dup2(null, 2);
        dup2(null, 0);  /* A command that wrongly reads its input ends instead of waiting for the keyboard */
        close(null);    /* The shell starts with 0, 1 and 2 only, as from a terminal */
        char *args[] = { "/bin/sh", "-c", (char *)command, NULL };
        execv("/bin/sh", args);
        exit(127);
    }
    close(p[1]);
    size_t used = 0;
    long n;
    while (used < size - 1 && (n = read(p[0], out + used, size - 1 - used)) > 0) used += (size_t)n;
    out[used] = '\0';
    close(p[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

/* Whether `sh -c command` prints exactly `expected` and succeeds */
static int sh_prints(const char *command, const char *expected) {
    static char out[512];
    return sh_capture(command, out, sizeof(out)) == 0 && strcmp(out, expected) == 0;
}

/* The shell's words: parts joined, single quotes literal, double quotes
 * expanding variables, separators and operators kept inside quotes */
static void check_shell_words(void) {
    static char out[64];
    CHECK(sh_prints("echo \"<\"x a\"b\"c", "<x abc\n"));
    CHECK(sh_prints("export X=val; echo 'single $X' \"double $X\" plain$X",
                    "single $X double val plainval\n"));
    CHECK(sh_prints("echo 'a;b' \"c && d\" 'e || f' '|<>'", "a;b c && d e || f |<>\n"));
    CHECK(sh_prints("echo \"it's\" 'say \"hi\"'", "it's say \"hi\"\n"));
    CHECK(sh_prints("echo \"\" x ''", " x \n"));                       /* Empty words are words */
    CHECK(sh_prints("echo '/bin/s*' \"/bin/s\"*", "/bin/s* /bin/s*\n"));  /* Quoted: no wildcards */
    CHECK(sh_capture("echo 'unclosed", out, sizeof(out)) == 2);          /* A syntax error */

    /* $(...): one word in double quotes, split outside them, nested,
     * with lists and pipes inside; its output is never run as commands */
    CHECK(sh_prints("echo [$(echo 'c   d')]", "[c d]\n"));                 /* Split: two words */
    CHECK(sh_prints("echo \"$(echo '1  2')\"", "1  2\n"));               /* Blanks kept in quotes */
    CHECK(sh_prints("echo x$(echo)y", "xy\n"));
    CHECK(sh_prints("echo $(echo $(echo nested))", "nested\n"));
    CHECK(sh_prints("echo $(true; echo listed) && echo after", "listed\nafter\n"));
    CHECK(sh_prints("echo $(echo one two | wc)", "1 2 8\n"));
    CHECK(sh_prints("echo $(echo '; echo injected')", "; echo injected\n"));
    CHECK(sh_prints("$(false) ; echo status $?", "status 1\n"));        /* Nothing left to run: its status */
    CHECK(sh_prints("echo out > $(echo /tmp/subst) && cat /tmp/subst && rm /tmp/subst", "out\n"));
    CHECK(sh_capture("echo x > $(echo two words)", out, sizeof(out)) == 2);
    CHECK(sh_capture("echo $(cat /bin/sh)", out, sizeof(out)) == 2);      /* More than 4 KiB */
    CHECK(sh_capture("echo $(echo unclosed", out, sizeof(out)) == 2);
}

/* Whether `sh -c command` prints exactly `expected` and ends with `status` */
static int sh_ends(const char *command, const char *expected, int status) {
    static char out[512];
    return sh_capture(command, out, sizeof(out)) == status && strcmp(out, expected) == 0;
}

/* The shell's redirections of any descriptor: N> N>> N< and N>&M, applied
 * left to right after the pipe (sh_capture sends standard error to
 * /dev/null, so whatever reaches the output went through a copy) */
static void check_shell_redirects(void) {
    static char out[128];
    CHECK(sh_capture("ls /no/such 2>&1", out, sizeof(out)) == 1 && strncmp(out, "ls: ", 4) == 0);
    CHECK(sh_capture("ls /no/such 2>&1 | cat", out, sizeof(out)) == 0 && strncmp(out, "ls: ", 4) == 0);
    CHECK(sh_capture("ls /no/such > /tmp/redir 2>&1; cat /tmp/redir", out, sizeof(out)) == 0 &&
          strncmp(out, "ls: ", 4) == 0);
    /* The order matters: here 2 copies the output before it is the file */
    CHECK(sh_capture("ls /no/such 2>&1 > /tmp/redir; echo end; cat /tmp/redir", out, sizeof(out)) == 0 &&
          strncmp(out, "ls: ", 4) == 0 && strcmp(out + strlen(out) - 4, "end\n") == 0);
    CHECK(sh_prints("echo e 1>&2", ""));
    CHECK(sh_prints("echo a 2>/tmp/redir 1>&2; echo b 2>>/tmp/redir 1>&2; cat /tmp/redir", "a\nb\n"));
    CHECK(sh_prints("echo 2>/tmp/redir x; cat /tmp/redir", "x\n"));     /* The digit is not a word */
    CHECK(sh_prints("echo x2>/tmp/redir; cat /tmp/redir", "x2\n"));     /* ... unless it is part of one */
    CHECK(sh_prints("echo w 3>/tmp/redir 1>&3; cat /tmp/redir", "w\n"));  /* open() picks 3 itself */
    CHECK(sh_prints("cat 3</tmp/redir 0<&3", "w\n"));
    CHECK(sh_ends("echo z 2>&7", "", 1));                               /* Not open */
    CHECK(sh_ends("cat 0<&3 3</tmp/redir", "", 1));                       /* Not yet open */
    CHECK(sh_ends("echo q 2>&1x", "", 2));
    CHECK(sh_ends("echo q 2>&", "", 2));
    CHECK(sh_ends("echo q 2>", "", 2));
    CHECK(unlink("/tmp/redir") == 0);
}

/* The shell's variables: NAME=value (the shell's own, unseen by programs),
 * export (seen), NAME=value program (for that program only), unset; the
 * values expanded but not split, and names that are not names */
static void check_shell_variables(void) {
    CHECK(sh_prints("X=1; echo [$X]", "[1]\n"));
    CHECK(sh_prints("X=hidden; sh -c 'echo [$X]'", "[]\n"));                 /* Not in the environment */
    CHECK(sh_prints("X=shown; export X; sh -c 'echo [$X]'", "[shown]\n"));   /* ... until exported */
    CHECK(sh_prints("export X=e; sh -c 'echo [$X]'", "[e]\n"));
    CHECK(sh_prints("export X=a; X=b; sh -c 'echo [$X]'", "[b]\n"));         /* Exported stays exported */
    CHECK(sh_prints("X=once sh -c 'echo [$X]'; echo [$X]", "[once]\n[]\n"));  /* For one program */
    CHECK(sh_prints("export X=kept; X=tmp sh -c 'echo $X'; echo $X", "tmp\nkept\n"));
    CHECK(sh_ends("PATH=/nowhere true", "", 127));                           /* The program's PATH is used */
    CHECK(sh_prints("A=1 B=2; echo $A$B", "12\n"));

    /* Values: expanded, neither split nor matched as wildcards */
    CHECK(sh_prints("X=\"a  b\"; echo \"$X\"", "a  b\n"));
    CHECK(sh_prints("X=$(echo 'p  q'); echo \"$X\"", "p  q\n"));
    CHECK(sh_prints("X=/bin/s*; echo \"$X\"", "/bin/s*\n"));
    CHECK(sh_prints("X=a=b; echo $X", "a=b\n"));
    CHECK(sh_prints("X=; echo [$X]", "[]\n"));

    /* The status: of the last $(...) in it, else 0; $? before it is still there */
    CHECK(sh_ends("X=$(false)", "", 1));
    CHECK(sh_prints("false; X=1; echo $?", "0\n"));
    CHECK(sh_prints("false; X=$?; echo $X", "1\n"));

    CHECK(sh_prints("X=1; export Y=2; unset X Y; echo [$X$Y]; sh -c 'echo [$Y]'", "[]\n[]\n"));
    CHECK(sh_prints("for v in 1; do sh -c 'echo [$v]'; done; echo $v", "[]\n1\n"));   /* The shell's own */

    /* Not assignments, or not names */
    CHECK(sh_ends("1x=y", "", 127));                                        /* A command */
    CHECK(sh_ends("\"X\"=1", "", 127));                                     /* Quoted: a command too */
    CHECK(sh_prints("echo X=1", "X=1\n"));                                  /* After the name: a word */
    CHECK(sh_ends("unset 1x", "", 1));
    CHECK(sh_ends("export a-b=1", "", 1));
    CHECK(sh_ends("X=1 | cat", "", 2));
    CHECK(sh_ends("X=1 > /tmp/never", "", 2));
}

/* Redirections and pipes after compound commands: every kind redirected,
 * piped from and into, 2>&1 after done, a pipe over two lines, the
 * command running in a child (what it sets stays there), and what is
 * still refused */
static void check_shell_compound_redirects(void) {
    CHECK(sh_prints("for x in a b; do echo $x; done > /tmp/cr; cat /tmp/cr", "a\nb\n"));
    CHECK(sh_prints("while true; do echo w; break; done>/tmp/cr; cat /tmp/cr", "w\n"));
    CHECK(sh_prints("until false; do echo u; break; done >> /tmp/cr; cat /tmp/cr", "w\nu\n"));
    CHECK(sh_prints("if true; then echo i; fi > /tmp/cr; cat /tmp/cr", "i\n"));
    CHECK(sh_prints("case x in x) echo c;; esac > /tmp/cr; cat /tmp/cr", "c\n"));
    CHECK(sh_prints("{ echo g1; echo g2; } > /tmp/cr; cat /tmp/cr", "g1\ng2\n"));
    CHECK(sh_prints("while true; do cat; break; done < /tmp/cr", "g1\ng2\n"));   /* Input too */
    CHECK(sh_prints("for x in 1 2 3; do echo $x; done | wc", "3 3 6\n"));
    CHECK(sh_prints("{ echo b; echo a; } | sort", "a\nb\n"));
    CHECK(sh_prints("if true; then echo yes; fi | tr a-z A-Z", "YES\n"));
    CHECK(sh_prints("echo in | while true; do cat; break; done", "in\n"));       /* Piped into */
    CHECK(sh_prints("echo q | { cat; } | { cat; }", "q\n"));
    CHECK(sh_prints("echo m | case a in a) cat;; esac | cat", "m\n"));
    CHECK(sh_prints("for x in 1; do ls /no/such; done 2>&1 | grep -c such", "1\n"));
    CHECK(sh_prints("for x in a b; do echo $x; done |\nsort -r", "b\na\n"));      /* The pipe ends a line */
    CHECK(sh_prints("X=out; for x in in; do X=$x; done | cat; echo $X", "out\n")); /* Ran in a child */
    CHECK(sh_prints("X=out; for x in in; do X=$x; done; echo $X", "in\n"));       /* ... unlike this */
    CHECK(sh_ends("for x in 1; do false; done | cat", "", 0));                  /* The last one's status */
    CHECK(sh_ends("{ echo a; } | false", "", 1));
    CHECK(sh_prints("if true; then\n  for x in 1 2; do echo $x; done > /tmp/cr\nfi; cat /tmp/cr", "1\n2\n"));

    CHECK(sh_ends("for x in 1; do echo z; done > /tmp/cr extra", "", 2));
    CHECK(sh_ends("for x in 1; do echo z; done >", "", 2));
    CHECK(sh_ends("for x in 1; do echo z; done junk", "", 2));
    CHECK(sh_ends("f() { echo f; } > /tmp/cr", "", 2));                          /* Not on a definition */
    CHECK(sh_ends("{ echo a; } | | cat", "", 2));
    CHECK(sh_ends("{ echo a; } < /no/such", "", 1));
    CHECK(unlink("/tmp/cr") == 0);
}

/* The shell's case (patterns, quoted parts literal, | and the optional
 * '(', empty and last items, over several lines), functions ($#, $1, "$@",
 * return, run in the shell or in a child, a body that redefines itself,
 * recursion and its limit, loops that stay the function's own) and ${NAME} */
static void check_shell_case_functions(void) {
    CHECK(sh_prints("x=hello; case $x in h*) echo H;; *) echo other;; esac", "H\n"));
    CHECK(sh_prints("case abc in \"*\") echo quoted;; a*) echo glob;; esac", "glob\n"));
    CHECK(sh_prints("case '*' in \"*\") echo star;; esac", "star\n"));
    CHECK(sh_prints("p='a*'; case abc in \"$p\") echo q;; $p) echo u;; esac", "u\n"));
    CHECK(sh_prints("case .x in *) echo dot;; esac", "dot\n"));                /* Not a file name */
    CHECK(sh_prints("case z in a|z) echo alt;; esac", "alt\n"));
    CHECK(sh_prints("case z in (z) echo paren;; esac", "paren\n"));
    CHECK(sh_prints("false; case q in a) echo no;; esac; echo s=$?", "s=0\n"));
    CHECK(sh_ends("case a in a) false;; esac", "", 1));
    CHECK(sh_prints("case a in a) ;; esac; echo empty", "empty\n"));
    CHECK(sh_prints("case b in\na)\n echo A\n ;;\nb)\n echo B1\n echo B2;;\nesac", "B1\nB2\n"));
    CHECK(sh_prints("case x\nin x) echo last; esac", "last\n"));                /* No ;; before esac */
    CHECK(sh_prints("for w in a b c; do case $w in b) continue;; esac; echo $w; done", "a\nc\n"));
    CHECK(sh_ends("case a in a echo;; esac", "", 2));
    CHECK(sh_ends("echo a;; echo b", "", 2));
    CHECK(sh_ends("case a in a) echo x;;", "", 2));                            /* No esac */
    CHECK(sh_ends("if true; then case a in a) echo x;; fi", "", 2));

    CHECK(sh_prints("f() { echo $# [$1] [$2]; }; f a 'b c'", "2 [a] [b c]\n"));
    CHECK(sh_prints("f() { return 3; echo never; }; f; echo s=$?", "s=3\n"));
    CHECK(sh_prints("f() { false; return; }; f; echo s=$?", "s=1\n"));
    CHECK(sh_prints("g() { for w in \"$@\"; do echo w=$w; done; }; g 1 '2 3'", "w=1\nw=2 3\n"));
    CHECK(sh_prints("g() { for w in \"$@\"; do echo w=$w; done; echo end; }; g", "end\n"));   /* No words */
    CHECK(sh_prints("f() {\n  echo multi\n}\nf", "multi\n"));
    CHECK(sh_prints("f()\n{\n echo brace below\n}\nf", "brace below\n"));
    CHECK(sh_prints("f() { X=set; }; f; echo $X", "set\n"));                    /* Run by the shell */
    CHECK(sh_prints("f() { echo piped $1; }; f x | cat", "piped x\n"));         /* By a child */
    CHECK(sh_prints("f() { echo out; }; f > /tmp/fn; cat /tmp/fn; rm /tmp/fn", "out\n"));
    CHECK(sh_prints("f() { echo $X; }; X=pre f", "pre\n"));
    CHECK(sh_prints("f() { echo $1; }; g() { f inner; echo $1; }; g mine", "inner\nmine\n"));
    CHECK(sh_prints("f() { echo one; }; f() { echo two; }; f", "two\n"));
    CHECK(sh_prints("f() { f() { echo redefined; }; echo first; }; f; f", "first\nredefined\n"));
    CHECK(sh_prints("f() { break; }; for i in 1 2; do f; echo $i; done", "1\n2\n"));
    CHECK(sh_prints("f() { for i in 1 2 3; do if [ $i = 2 ]; then return 7; fi; echo $i; done; echo no; }; f; echo s=$?",
                    "1\ns=7\n"));
    CHECK(sh_prints("r() { case $1 in xxx) echo deep $1;; *) r x$1;; esac; }; r ''", "deep xxx\n"));
    CHECK(sh_ends("inf() { inf; }; inf; echo never", "", 2));                  /* The depth limit */
    CHECK(sh_prints("sh -c 'inf() { inf; }; inf'; echo survived $?", "survived 2\n"));
    CHECK(sh_prints("f() { echo fn; }; sh -c f; echo s=$?", "s=127\n"));        /* Programs do not see it */
    CHECK(sh_ends("return", "", 1));
    CHECK(sh_ends("f() { return x; }; f", "", 2));
    CHECK(sh_ends("f() { }", "", 2));
    CHECK(sh_ends("f() echo", "", 2));
    CHECK(sh_prints("{ echo g; false; }; echo s=$?", "g\ns=1\n"));

    CHECK(sh_prints("x=ab; echo ${x}c $xc. \"${x}\"", "abc . ab\n"));
    CHECK(sh_prints("echo $0 $# [$1]", "sh 0 []\n"));
    CHECK(sh_ends("echo ${x", "", 2));
    CHECK(sh_ends("echo ${}", "", 2));
}

/* Make `path` hold `size` bytes of `text`; whether it worked */
static int put_file(const char *path, const char *text, size_t size) {
    long fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    int ok = write((int)fd, text, size) == (long)size;
    close((int)fd);
    return ok;
}

/* Here-documents: expanded unless the word is quoted, <<- taking tabs off,
 * several on a line, in a pipeline, a loop, a function and after a
 * compound command, one larger than a pipe holds; and '&' after a
 * compound command, which needs the terminal */
static void check_shell_heredocs(void) {
    CHECK(sh_prints("X=v; cat <<EOF\nhello $X ${X}y $((2 + 3)) $(echo sub)\nEOF", "hello v vy 5 sub\n"));
    CHECK(sh_prints("X=v; cat <<'EOF'\nraw $X $(echo no)\nEOF", "raw $X $(echo no)\n"));
    CHECK(sh_prints("X=v; cat <<E\"O\"F\npart $X\nEOF", "part $X\n"));            /* Any quoted part */
    CHECK(sh_prints("X=v; cat <<\\E\nslash $X\nE", "slash $X\n"));
    CHECK(sh_prints("X=v; cat <<E\n\\$X \\\\ a\\\nb \\q\nE", "$X \\ ab \\q\n"));  /* \$ \\ \newline; \q kept */
    CHECK(sh_prints("cat <<E\na;b 'q' \"d\" # kept | && <x\nE x\n  E\nE", "a;b 'q' \"d\" # kept | && <x\nE x\n  E\n"));
    CHECK(sh_prints("cat <<-E\n\tone\n\t\ttwo\n\tE\necho after", "one\ntwo\nafter\n"));
    CHECK(sh_prints("cat <<A; cat <<B; echo end\nfirst\nA\nsecond\nB", "first\nsecond\nend\n"));
    CHECK(sh_prints("cat <<E | sort\nb\na\nE", "a\nb\n"));
    CHECK(sh_prints("for i in 1 2; do cat <<E\nround $i\nE\ndone", "round 1\nround 2\n"));    /* Expanded each time */
    CHECK(sh_prints("while read l; do echo [$l]; done <<E\nx\ny z\nE", "[x]\n[y z]\n"));
    CHECK(sh_prints("f() { cat <<E\n$# $@\nE\n}; f a b; f c", "2 a b\n1 c\n"));
    CHECK(sh_prints("read a b <<E\none two three\nE\necho $a/$b", "one/two three\n"));     /* read: in the shell */
    CHECK(sh_prints("cat 3<<E 0<&3\nthree\nE", "three\n"));
    CHECK(sh_prints("cat <<E\nE", ""));

    /* 6000 bytes, more than the pipe holds, so written by a process of its
     * own: made by expanding a variable twice (a command line or a script's
     * command may not be that long: 4 KiB of arguments, 4 KiB pending) */
    static char lines[3000];
    for (int line = 0; line < 30; line++) {
        memset(lines + line * 100, 'x', 99);
        lines[line * 100 + 99] = '\n';
    }
    CHECK(put_file("/tmp/hd", lines, sizeof(lines)));
    CHECK(sh_prints("X=$(cat /tmp/hd); cat <<E | wc\n$X\n$X\nE", "60 60 6000\n"));
    CHECK(unlink("/tmp/hd") == 0);

    CHECK(sh_ends("cat <<E\nno end", "", 2));                 /* The text ended before WORD */
    CHECK(sh_ends("cat <<", "", 2));
    CHECK(sh_ends("cat <<'E\nx\nE", "", 2));
    CHECK(sh_ends("cat <<E\n$(echo unclosed\nE", "", 1));     /* Fails when it is expanded */
    CHECK(sh_ends("while false; do true; done &", "", 2));     /* '&': only at the terminal */
    CHECK(sh_ends("{ echo a; } & x", "", 2));
}

/* The shell's $((...)) (precedence, signs, bases, wrapping, variables,
 * && and || that skip, the errors) and read (fields, the rest, -r and
 * backslashes, the end of input, from a file, a pipe and a loop) */
static void check_shell_arith_read(void) {
    CHECK(sh_prints("echo $((1 + 2 * 3)) $(( (1 + 2) * 3 )) $((10 - 4 - 3)) $((2 * 3 % 4))", "7 9 3 2\n"));
    CHECK(sh_prints("echo $((-7 / 2)) $((-7 % 2)) $((- -3)) $((+4)) $((!0)) $((!5))", "-3 -1 3 4 1 0\n"));
    CHECK(sh_prints("echo $((0x1f)) $((010)) $((1 < 2)) $((2 <= 1)) $((3 == 3)) $((3 != 3)) $((2 >= 2))",
                    "31 8 1 0 1 0 1\n"));
    CHECK(sh_prints("echo $((9223372036854775807 + 1)) $((-9223372036854775807 - 1 - 1))",
                    "-9223372036854775808 9223372036854775807\n"));        /* Wraps, as said */
    CHECK(sh_prints("echo $((-9223372036854775807 - 1)) $(((-9223372036854775807 - 1) / -1))",
                    "-9223372036854775808 -9223372036854775808\n"));
    CHECK(sh_prints("i=4; echo $((i * i)) $(($i + 1)) $((${i} - 1)) $((unset_x + 1))", "16 5 3 1\n"));
    CHECK(sh_prints("n=-3; s=' 7 '; e=; echo $((n * 2)) $((s + 1)) $((e))", "-6 8 0\n"));
    CHECK(sh_prints("i=0; while [ $i -lt 5 ]; do i=$((i + 1)); done; echo $i", "5\n"));
    CHECK(sh_prints("f() { echo $(($1 + $2)) $(($#)); }; f 40 2", "42 2\n"));
    CHECK(sh_prints("false; echo $(($? * 10))", "10\n"));
    CHECK(sh_prints("echo \"[$((1+1))]\" x$((2))y", "[2] x2y\n"));
    CHECK(sh_prints("x=0; echo $((x != 0 && 10 / x)) $((x == 0 || 10 / x))", "0 1\n"));  /* Not evaluated */
    CHECK(sh_prints("x=abc; echo $((0 && x))", "0\n"));
    CHECK(sh_prints("echo \"$(echo a)\"; echo b", "a\nb\n"));                /* ')' inside quotes */
    CHECK(sh_ends("echo $((1 / 0))", "", 2));
    CHECK(sh_ends("echo $((5 % 0))", "", 2));
    CHECK(sh_ends("x=abc; echo $((x + 1))", "", 2));
    CHECK(sh_ends("echo $((1 +))", "", 2));
    CHECK(sh_ends("echo $((1 2))", "", 2));
    CHECK(sh_ends("echo $(((1 + 2))", "", 2));
    CHECK(sh_ends("echo $((08))", "", 2));
    CHECK(sh_ends("echo $((99999999999999999999))", "", 2));
    CHECK(sh_ends("echo $((1 / 0)) never; echo after", "after\n", 0));      /* Only that command fails */

    static const char rd[] = "one two three four\n  lead  trail  \nlast";
    CHECK(put_file("/tmp/rd", rd, strlen(rd)));
    CHECK(sh_prints("read a b < /tmp/rd; echo [$a][$b]", "[one][two three four]\n"));
    CHECK(sh_prints("read a b c d e < /tmp/rd; echo [$a][$d][$e]", "[one][four][]\n"));
    CHECK(sh_prints("while read l; do echo [$l]; done < /tmp/rd", "[one two three four]\n[lead  trail]\n"));
    CHECK(sh_prints("{ read x; read y z; read w; echo [$x][$y][$z][$w] $?; } < /tmp/rd",
                    "[one two three four][lead][trail][last] 1\n"));            /* Last line, no '\n': 1 */
    CHECK(sh_prints("echo 'a b' | { read p q; echo [$p][$q]; }", "[a][b]\n"));
    CHECK(sh_prints("x=kept; echo new | read x; echo $x", "kept\n"));      /* In a child: lost */
    CHECK(sh_prints("echo 'a\\ b c\\\\d' | { read p q; echo [$p][$q]; }", "[a b][c\\d]\n"));
    CHECK(sh_prints("echo 'a\\ b c' | { read -r p q; echo [$p][$q]; }", "[a\\][b c]\n"));
    CHECK(sh_prints("printf_free() { echo 'jo\\'; echo 'ined'; }; printf_free | { read v; echo $v; }", "joined\n"));
    CHECK(sh_ends("read v; echo [$v]", "[]\n", 0));                         /* Empty input: the echo's status */
    CHECK(sh_ends("read v", "", 1));
    CHECK(sh_ends("read", "", 2));
    CHECK(sh_ends("read 1x", "", 2));
    CHECK(sh_ends("read v < /no/such", "", 1));
    CHECK(sh_prints("read v < /no/such; read w < /tmp/rd; echo $w", "one two three four\n"));  /* Input put back */
    CHECK(unlink("/tmp/rd") == 0);
}

/* uniq, cut and tr: their basic forms, a line longer than any fixed buffer
 * (two equal ones, then a last line without '\n'), and what they refuse */
static void check_line_tools(void) {
    static char out[128];
    CHECK(put_file("/tmp/lt", "a\na\nb\na\nc\nc", 11));                  /* No '\n' at the end */
    CHECK(sh_prints("uniq /tmp/lt", "a\nb\na\nc\n"));
    CHECK(sh_prints("uniq -c /tmp/lt", "2 a\n1 b\n1 a\n2 c\n"));
    CHECK(sh_prints("uniq -d /tmp/lt", "a\nc\n"));
    CHECK(sh_prints("cat /tmp/lt | uniq -c -d", "2 a\n2 c\n"));

    CHECK(put_file("/tmp/lt", "one:two:three:four\nplain\ntab\tsep", 32));
    CHECK(sh_prints("cut -d : -f 2 /tmp/lt", "two\nplain\ntab\tsep\n"));    /* No ':': the whole line */
    CHECK(sh_prints("cut -d : -f 3-,1,1 /tmp/lt", "one:three:four\nplain\ntab\tsep\n"));
    CHECK(sh_prints("cut -d : -f -2 /tmp/lt | head -n 1", "one:two\n"));
    CHECK(sh_prints("cut -f 2 /tmp/lt", "one:two:three:four\nplain\nsep\n"));  /* A tab by default */
    CHECK(sh_prints("cut -c 2-4,1 /tmp/lt", "one:\nplai\ntab\t\n"));
    CHECK(sh_prints("cut -c 9- /tmp/lt", "three:four\n\n\n"));

    CHECK(sh_prints("echo Hello, World | tr a-z A-Z", "HELLO, WORLD\n"));
    CHECK(sh_prints("echo hello | tr -d l", "heo\n"));
    CHECK(sh_prints("echo hello | tr a-z x", "xxxxx\n"));                   /* SET2's last, repeated */
    CHECK(sh_prints("echo a b c | tr ' ' '\\n'", "a\nb\nc\n"));

    /* 6000 bytes, the same again, then "y" with no '\n' */
    static char big[12003];
    memset(big, 'x', sizeof(big));
    big[6000] = '\n';
    big[12001] = '\n';
    big[12002] = 'y';
    CHECK(put_file("/tmp/lt", big, sizeof(big)));
    CHECK(sh_prints("uniq -c /tmp/lt | cut -c 1-4", "2 xx\n1 y\n"));
    CHECK(sh_prints("cut -c 5999- /tmp/lt", "xx\nxx\n\n"));
    CHECK(sh_prints("tr x z < /tmp/lt | cut -c 1,6000-", "zz\nzz\ny\n"));
    CHECK(sh_prints("tr -d x < /tmp/lt", "\n\ny"));
    big[3000] = 'w';                                       /* Equal length, not equal */
    CHECK(put_file("/tmp/lt", big, sizeof(big)));
    CHECK(sh_prints("uniq -c /tmp/lt | cut -c 1-2", "1 \n1 \n1 \n"));

    CHECK(sh_capture("cut -c 0 /tmp/lt", out, sizeof(out)) == 2);
    CHECK(sh_capture("cut -c 3-1 /tmp/lt", out, sizeof(out)) == 2);
    CHECK(sh_capture("cut -d : -c 1 /tmp/lt", out, sizeof(out)) == 2);     /* -d goes with -f */
    CHECK(sh_capture("cut -f 1 /no/such", out, sizeof(out)) == 1);
    CHECK(sh_capture("tr z-a b < /tmp/lt", out, sizeof(out)) == 2);         /* A range backwards */
    CHECK(sh_capture("tr -d < /tmp/lt", out, sizeof(out)) == 2);
    CHECK(sh_capture("uniq -x /tmp/lt", out, sizeof(out)) == 2);
    CHECK(sh_capture("uniq /no/such", out, sizeof(out)) == 1);
    CHECK(unlink("/tmp/lt") == 0);
}

/* /etc/hosts: the lookup on a file written for it (the first line naming a
 * name wins; case, comments, tabs, a carriage return, a line that does not
 * start with an address, one too long to hold, a last line without '\n'),
 * then resolve() on the real file and, for a name not in it, falling
 * through to the DNS: a server in a child, made the configured one */
static void check_hosts_file(void) {
    static char text[700];
    uint32_t ip = 0;
    size_t len = 0;
    const char *head = "# 10.6.6.6 commented\n10.9.8.7 alpha beta # gamma\nnot-an-address delta\n"
                       "10.9.8.6 alpha\n\t10.5.5.5\tTabbed\r\n10.1.1.1 ";
    memcpy(text, head, strlen(head));
    len += strlen(head);
    memset(text + len, 'x', 300);                            /* A name too long for any line buffer */
    len += 300;
    const char *tail = " zeta\n10.4.4.4 last";              /* No '\n' at the end */
    memcpy(text + len, tail, strlen(tail));
    len += strlen(tail);
    CHECK(put_file("/tmp/hosts", text, len));
    CHECK(hosts_lookup("/tmp/hosts", "alpha", &ip) == 0 && ip == parse_ip("10.9.8.7"));  /* The first one */
    CHECK(hosts_lookup("/tmp/hosts", "BETA", &ip) == 0 && ip == parse_ip("10.9.8.7"));
    CHECK(hosts_lookup("/tmp/hosts", "tabbed", &ip) == 0 && ip == parse_ip("10.5.5.5"));
    CHECK(hosts_lookup("/tmp/hosts", "last", &ip) == 0 && ip == parse_ip("10.4.4.4"));
    CHECK(hosts_lookup("/tmp/hosts", "gamma", &ip) == -ENOENT);       /* In a comment */
    CHECK(hosts_lookup("/tmp/hosts", "commented", &ip) == -ENOENT);
    CHECK(hosts_lookup("/tmp/hosts", "delta", &ip) == -ENOENT);       /* No address before it */
    CHECK(hosts_lookup("/tmp/hosts", "zeta", &ip) == -ENOENT);        /* On a line too long */
    CHECK(hosts_lookup("/tmp/hosts", "10.9.8.7", &ip) == -ENOENT);    /* An address is not a name */
    CHECK(hosts_lookup("/tmp/hosts", "", &ip) == -ENOENT);
    CHECK(hosts_lookup("/no/such", "alpha", &ip) == -ENOENT);
    CHECK(unlink("/tmp/hosts") == 0);

    /* The system's file, through resolve() */
    CHECK(resolve("localhost", &ip) == 0 && ip == parse_ip("127.0.0.1"));

    /* A name the file does not have goes to the configured DNS server: one
     * on this machine, answering one question with 198.51.100.4. A name the
     * file has must not reach it (or the server's answer would be taken). */
    struct net_config config, here;
    CHECK(netconfig(&config, NULL) == 0);
    long server = socket(SOCK_DGRAM);
    CHECK(server >= 3 && bind((int)server, 53) == 0);
    long pid = fork();
    if (pid == 0) {
        static uint8_t msg[512];
        struct net_address from;
        long n = recvfrom((int)server, msg, sizeof(msg) - 16, &from, 5000);
        if (n < 12) exit(1);
        static const uint8_t answer[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 198, 51, 100, 4 };
        msg[2] = 0x81; msg[3] = 0x80;                     /* An answer, no error */
        msg[6] = 0; msg[7] = 1;                           /* One answer record */
        memcpy(msg + n, answer, sizeof(answer));
        exit(sendto((int)server, msg, (size_t)n + sizeof(answer), from.ip, from.port) > 0 ? 0 : 2);
    }
    here = config;
    here.dns = OUR_IP;
    CHECK(netconfig(NULL, &here) == 0);
    CHECK(resolve("gateway", &ip) == 0 && ip == parse_ip("10.0.2.2"));              /* From the file */
    CHECK(resolve("fallthrough.test", &ip) == 0 && ip == parse_ip("198.51.100.4")); /* From the server */
    CHECK(netconfig(NULL, &config) == 0);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(close((int)server) == 0);
}

/* The options of grep, sort, head, tail, wc and uniq, read by one parser
 * (user/lib/opts.h): grouped and separate, values joined and apart, "--",
 * and the refusals; sort -k on columns of different widths, -n with
 * negative numbers, fractions and blanks */
static void check_tool_options(void) {
    static char out[256];
    const char *t = "b 10\na  2\nc -3\nA 2\nd 1.5\n";
    CHECK(put_file("/tmp/to", t, strlen(t)));
    CHECK(sh_prints("grep -in a /tmp/to", "2:a  2\n4:A 2\n"));
    CHECK(sh_prints("grep -i -n a /tmp/to", "2:a  2\n4:A 2\n"));
    CHECK(sh_prints("grep -cv a /tmp/to", "4\n"));                     /* 'A' is not 'a' without -i */
    CHECK(sh_prints("grep -civ a /tmp/to", "3\n"));
    CHECK(sh_prints("grep -l 10 /tmp/to /etc/motd", "/tmp/to\n"));
    CHECK(sh_prints("grep -lc 10 /tmp/to", "/tmp/to\n"));              /* -l wins over -c */
    CHECK(sh_prints("grep -nc 10 /tmp/to /etc/motd", "/tmp/to:1\n/etc/motd:0\n"));
    CHECK(sh_prints("grep -- -3 /tmp/to", "c -3\n"));                  /* "--" ends the options */
    CHECK(sh_ends("grep -x a /tmp/to", "", 2));
    CHECK(sh_ends("grep -i", "", 2));

    /* sort: numbers with signs, fractions and blanks before them */
    CHECK(sh_prints("sort -k 2 -n /tmp/to", "c -3\nd 1.5\na  2\nA 2\nb 10\n"));
    CHECK(sh_prints("sort -nk2 -r /tmp/to", "b 10\na  2\nA 2\nd 1.5\nc -3\n"));  /* Equal keys keep their order */
    CHECK(sh_prints("sort -u -k 2 -n /tmp/to", "c -3\nd 1.5\na  2\nb 10\n"));
    CHECK(sh_prints("sort -t ' ' -k 1,1 -r /tmp/to", "d 1.5\nc -3\nb 10\na  2\nA 2\n"));
    const char *w = "x:30:b\nyy:4:a\nz:100:c\n";                       /* Fields of different widths */
    CHECK(put_file("/tmp/to2", w, strlen(w)));
    CHECK(sh_prints("sort -t : -k 2,2 -n /tmp/to2", "yy:4:a\nx:30:b\nz:100:c\n"));
    CHECK(sh_prints("sort -t: -k3 /tmp/to2", "yy:4:a\nx:30:b\nz:100:c\n"));
    CHECK(sh_prints("sort -t : -k 2,2 /tmp/to2", "z:100:c\nx:30:b\nyy:4:a\n"));   /* As text */
    CHECK(sh_prints("echo 123456789012345678901234567890 > /tmp/to3; echo 99 >> /tmp/to3; sort -n /tmp/to3",
                    "99\n123456789012345678901234567890\n"));         /* Longer than a long */
    CHECK(sh_prints("sort /tmp/to2 /tmp/to3 | head -n 1", "123456789012345678901234567890\n"));  /* Two files */
    CHECK(sh_ends("sort -k 0 /tmp/to", "", 2));
    CHECK(sh_ends("sort -k 2,1 /tmp/to", "", 2));
    CHECK(sh_ends("sort -t ab /tmp/to", "", 2));
    CHECK(sh_ends("sort -k", "", 2));

    /* head and tail by bytes, and N joined to its option */
    CHECK(sh_prints("head -c 6 /tmp/to", "b 10\na"));
    CHECK(sh_prints("head -n2 /tmp/to", "b 10\na  2\n"));
    CHECK(sh_prints("tail -c 4 /tmp/to", "1.5\n"));
    CHECK(sh_prints("tail -c 100 /tmp/to2", "x:30:b\nyy:4:a\nz:100:c\n"));
    CHECK(sh_prints("tail -n1 /tmp/to2", "z:100:c\n"));
    CHECK(sh_prints("cat /tmp/to | tail -c 3", ".5\n"));
    CHECK(sh_ends("head -n x /tmp/to", "", 2));
    CHECK(sh_ends("head -c -1 /tmp/to", "", 2));
    CHECK(sh_ends("tail -c 2000000 /tmp/to", "", 2));

    /* wc: what is asked for, in its fixed order; names with several files */
    CHECK(sh_prints("wc -l /tmp/to", "5\n"));
    CHECK(sh_prints("wc -cw /tmp/to", "10 25\n"));                       /* Words before bytes */
    CHECK(sh_prints("wc -l /tmp/to /tmp/to2", "5 /tmp/to\n3 /tmp/to2\n8 total\n"));
    CHECK(sh_prints("wc /tmp/to", "5 10 25\n"));
    CHECK(sh_ends("wc -x /tmp/to", "", 2));

    /* uniq -i: runs that differ only in case; the first one is shown */
    CHECK(put_file("/tmp/to", "AB\nab\nAb\nb\nB\n", 13));
    CHECK(sh_prints("uniq -ic /tmp/to", "3 AB\n2 b\n"));
    CHECK(sh_prints("uniq -i -d /tmp/to", "AB\nb\n"));
    CHECK(sh_prints("uniq /tmp/to | wc -l", "5\n"));
    CHECK(sh_capture("uniq -q /tmp/to", out, sizeof(out)) == 2);
    CHECK(unlink("/tmp/to") == 0 && unlink("/tmp/to2") == 0 && unlink("/tmp/to3") == 0);
}

/* tee, xargs, seq, basename and dirname: their forms, POSIX's edge cases
 * for the two name tools, and xargs keeping every command line within
 * what execve takes (32 arguments, 4096 bytes with the environment) */
static void check_more_tools(void) {
    static char out[128];
    CHECK(sh_prints("basename /a/b.c .c; basename a/b/; basename /; basename //; basename ''",
                    "b\nb\n/\n/\n\n"));
    CHECK(sh_prints("basename .c .c; basename x/y.c y.c; basename a.c x.c", ".c\ny.c\na.c\n"));  /* Not the whole name */
    CHECK(sh_prints("dirname /a//b; dirname /a; dirname a; dirname ///; dirname a/b/; dirname ''",
                    "/a\n/\n.\n/\na\n.\n"));
    CHECK(sh_ends("basename", "", 2));
    CHECK(sh_ends("dirname a b", "", 2));

    CHECK(sh_prints("seq 3; seq -1 1; seq 10 -4 1", "1\n2\n3\n-1\n0\n1\n10\n6\n2\n"));
    CHECK(sh_prints("seq 0; seq 5 1", ""));                               /* Already past the end */
    CHECK(sh_ends("seq 1 0 5", "", 2));
    CHECK(sh_ends("seq 1x", "", 2));
    CHECK(sh_ends("seq", "", 2));

    CHECK(sh_prints("echo hi | tee /tmp/tee1 /tmp/tee2 && cat /tmp/tee1 /tmp/tee2", "hi\nhi\nhi\n"));
    CHECK(sh_prints("echo more | tee -a /tmp/tee1 > /dev/null; cat /tmp/tee1", "hi\nmore\n"));
    CHECK(sh_prints("echo x | tee /no/such/f /tmp/tee2 > /dev/null; echo $?; cat /tmp/tee2", "1\nx\n"));
    CHECK(sh_ends("tee /tmp/tee1 < /bin/sh > /tmp/tee2", "", 0));          /* Many reads' worth */
    CHECK(same_contents("/bin/sh", "/tmp/tee1") && same_contents("/bin/sh", "/tmp/tee2"));
    CHECK(unlink("/tmp/tee1") == 0 && unlink("/tmp/tee2") == 0);

    CHECK(sh_prints("seq 5 | xargs; seq 5 | xargs -n 2 echo n", "1 2 3 4 5\nn 1 2\nn 3 4\nn 5\n"));
    CHECK(put_file("/tmp/xw", "a\tb\n\n  c d", 11));                      /* Blanks of every kind, no final '\n' */
    CHECK(sh_prints("xargs < /tmp/xw", "a b c d\n"));
    CHECK(sh_prints("echo | xargs echo never", ""));                       /* No words: nothing run */
    CHECK(sh_prints("seq 100 | xargs | wc", "4 100 292\n"));               /* 31 words a line at most */
    CHECK(sh_ends("echo a | xargs nosuch", "", 127));
    CHECK(sh_ends("seq 3 | xargs false", "", 123));
    CHECK(sh_ends("seq 3 | xargs -n 0", "", 2));

    /* 40 words of 300 bytes: no line may pass 4096 bytes with the
     * environment, or execve refuses it (and xargs ends with 126). With a
     * 1000-byte variable, ten words fit and thirteen would without it. */
    static char big[40 * 301];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (i % 301 == 300) ? '\n' : 'w';
    CHECK(put_file("/tmp/xw", big, sizeof(big)));
    CHECK(sh_capture("xargs < /tmp/xw | wc", out, sizeof(out)) == 0 && strlen(out) > 10 &&
          strcmp(out + strlen(out) - 10, " 40 12040\n") == 0 && out[0] >= '3' && out[0] <= '9');
    static char value[1000];
    memset(value, 'e', sizeof(value));
    CHECK(put_file("/tmp/xe", value, sizeof(value)));
    CHECK(sh_prints("export E=$(cat /tmp/xe); xargs < /tmp/xw | wc", "4 40 12040\n"));
    CHECK(unlink("/tmp/xe") == 0);
    for (size_t i = 4500; i < 9500; i++) big[i] = 'w';                     /* One word of over 5000 bytes */
    CHECK(put_file("/tmp/xw", big, sizeof(big)));
    CHECK(sh_capture("xargs < /tmp/xw", out, sizeof(out)) == 1);
    CHECK(unlink("/tmp/xw") == 0);
}

/* What lstat says of `path`: its group, and its mode in *mode (if not
 * NULL); -1 if it cannot be described */
static long group_of(const char *path, unsigned *mode) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (mode) *mode = st.mode;
    return st.gid;
}

/* `dir` followed by `rest`, in `out` (96 bytes; the paths here are short) */
static const char *join(char *out, const char *dir, const char *rest) {
    size_t d = strlen(dir), r = strlen(rest);
    if (d + r >= 96) return "/too-long";
    memcpy(out, dir, d);
    memcpy(out + d, rest, r + 1);
    return out;
}

/* In the set-gid directory `dir` (group 50): a new file, directory and link
 * take its group, a new directory takes the bit too, and that passes
 * further down; a file gets no bit; in a directory without the bit, the
 * creator's group (`own_gid`). How many of these did not hold. */
static int setgid_directory_rule(const char *dir, unsigned own_gid) {
    char path[96];
    int failed = 0;
    unsigned mode = 0;
    long fd = open(join(path, dir, "/file"), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || close((int)fd) != 0 || group_of(path, NULL) != 50) failed++;
    if (mkdir(join(path, dir, "/sub")) != 0 || group_of(path, &mode) != 50 || !(mode & 02000)) failed++;
    if (symlink("file", join(path, dir, "/link")) != 0 || group_of(path, NULL) != 50) failed++;
    if (mkdir(join(path, dir, "/sub/deeper")) != 0 || group_of(path, &mode) != 50 || !(mode & 02000)) failed++;
    fd = open(join(path, dir, "/sub/deeper/file"), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || close((int)fd) != 0 || group_of(path, &mode) != 50 || (mode & 02000)) failed++;
    /* Without the bit, the creator's group */
    if (chmod(join(path, dir, "/sub/deeper"), 0777) != 0) failed++;
    fd = open(join(path, dir, "/sub/deeper/plain"), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || close((int)fd) != 0 || group_of(path, NULL) != (long)own_gid) failed++;
    return failed;
}

/* rm -r `dir` through the shell: its exit status */
static int remove_tree(const char *dir) {
    static char command[128], out[64];
    memcpy(command, "rm -r ", 6);
    size_t n = strlen(dir);
    if (n >= sizeof(command) - 6) return -1;
    memcpy(command + 6, dir, n + 1);
    return sh_capture(command, out, sizeof(out));
}

/* The set-gid directory rule as alice (group 100), in one everyone may
 * write: it is not a privilege of root's */
static void setgid_directory_as_alice(void) {
    CHECK(setgid_directory_rule("/tmp/sga", 100) == 0);
}

/* mkdir -p, rmdir, rm -r (links removed, never followed), and set-gid
 * directories, on the memory filesystem and on ext2 */
static void check_directory_tools(void) {
    static char out[256];
    struct stat st;
    CHECK(sh_prints("mkdir -p /tmp/tt/a/b/c/d && mkdir -p /tmp/tt/a/b /tmp/tt//e/ && echo ok", "ok\n"));
    CHECK(stat("/tmp/tt/a/b/c/d", &st) == 0 && st.type == S_DIR);
    CHECK(stat("/tmp/tt/e", &st) == 0 && st.type == S_DIR);
    CHECK(sh_ends("mkdir /tmp/tt/a", "", 1));                              /* Exists: only -p accepts it */
    CHECK(put_file("/tmp/tt/f", "x", 1));
    CHECK(sh_ends("mkdir -p /tmp/tt/f/x", "", 1));                         /* A file on the way */
    CHECK(sh_ends("mkdir -p /tmp/tt/f", "", 1));                           /* ... or at the end */
    CHECK(sh_ends("mkdir -z /tmp/tt/z", "", 2));

    /* rmdir: empty directories only, not files, not links to directories */
    CHECK(symlink("/tmp/tt/e", "/tmp/tt/to-e") == 0);
    CHECK(sh_ends("rmdir /tmp/tt/a", "", 1) && stat("/tmp/tt/a", &st) == 0);
    CHECK(sh_ends("rmdir /tmp/tt/f", "", 1) && stat("/tmp/tt/f", &st) == 0);
    CHECK(sh_ends("rmdir /tmp/tt/to-e", "", 1) && lstat("/tmp/tt/to-e", &st) == 0 && stat("/tmp/tt/e", &st) == 0);
    CHECK(sh_prints("rmdir /tmp/tt/a/b/c/d && echo gone", "gone\n") && stat("/tmp/tt/a/b/c/d", &st) == -ENOENT);

    /* rm -r: the whole tree, but not what a link in it points at */
    CHECK(mkdir("/tmp/keep") == 0 && put_file("/tmp/keep/k", "kept", 4));
    CHECK(symlink("/tmp/keep", "/tmp/tt/a/b/outside") == 0);
    CHECK(symlink("/tmp/keep/k", "/tmp/tt/a/k-link") == 0);
    CHECK(sh_ends("rm /tmp/tt/a", "", 1));                                 /* Without -r: not a full directory */
    CHECK(sh_prints("rm -r /tmp/tt && echo removed", "removed\n"));
    CHECK(lstat("/tmp/tt", &st) == -ENOENT);
    CHECK(stat("/tmp/keep/k", &st) == 0 && st.size == 4);
    CHECK(sh_ends("rm -r /", "", 1) && stat("/bin/sh", &st) == 0);
    CHECK(sh_ends("rm -r ///", "", 1));
    CHECK(sh_ends("rm -r /tmp/keep/..", "", 1) && stat("/tmp/keep/k", &st) == 0);
    CHECK(sh_ends("cd /tmp/keep && rm -r .", "", 1) && stat("/tmp/keep/k", &st) == 0);
    CHECK(sh_ends("rm -r /tmp/keep/./", "", 1));
    CHECK(sh_ends("rm -r /tmp/no-such", "", 1));
    CHECK(sh_ends("rm -x /tmp/keep", "", 2));
    CHECK(sh_prints("rm -r /tmp/keep/k && rm -r /tmp/keep && echo ok", "ok\n") && stat("/tmp/keep", &st) == -ENOENT);

    /* Set-gid directories, on the memory filesystem (root's group is 0) */
    CHECK(mkdir("/tmp/sg") == 0 && chown("/tmp/sg", KEEP_ID, 50) == 0 && chmod("/tmp/sg", 02775) == 0);
    CHECK(setgid_directory_rule("/tmp/sg", 0) == 0);
    CHECK(sh_capture("ls -l /tmp | grep ' sg'", out, sizeof(out)) == 0 && strncmp(out, "drwxrwsr-x", 10) == 0);
    CHECK(remove_tree("/tmp/sg") == 0 && stat("/tmp/sg", &st) == -ENOENT);
    CHECK(mkdir("/tmp/sga") == 0 && chown("/tmp/sga", KEEP_ID, 50) == 0 && chmod("/tmp/sga", 02777) == 0);
    CHECK(as_user_in_group(1000, 100, setgid_directory_as_alice) == 0);
    CHECK(remove_tree("/tmp/sga") == 0);

    /* ... and on the system disk, when it is the root (ext2 stores the group) */
    if (stat("/", &st) == 0 && st.dev == 3) {
        CHECK(mkdir("/sg-check") == 0 && chown("/sg-check", KEEP_ID, 50) == 0 && chmod("/sg-check", 02775) == 0);
        CHECK(setgid_directory_rule("/sg-check", 0) == 0);
        CHECK(remove_tree("/sg-check") == 0 && stat("/sg-check", &st) == -ENOENT);
    }
}

/* set -- ARGS, $* and "$*", getopts */
static void check_shell_arguments(void) {
    CHECK(sh_prints("set -- a 'b c' d; echo $# $1 $2 $3", "3 a b c d\n"));
    CHECK(sh_prints("set -- a b; set --; echo [$#] [$1]", "[0] []\n"));               /* None */
    CHECK(sh_prints("set x y; echo $2 $1", "y x\n"));                                    /* set ARGS */
    CHECK(sh_prints("set -- -e x; echo $1 $2", "-e x\n"));                               /* After --: arguments, not options */
    CHECK(sh_prints("set -- 'a b' c; for w in \"$@\"; do echo [$w]; done", "[a b]\n[c]\n"));
    CHECK(sh_prints("set -- 'a b' c; for w in \"$*\"; do echo [$w]; done", "[a b c]\n"));
    CHECK(sh_prints("set -- 'a b' c; for w in $*; do echo [$w]; done", "[a b]\n[c]\n"));   /* Unquoted: as $@ (values are not split) */
    CHECK(sh_prints("f() { set -- in f; echo $2; }; set -- out; f; echo $1", "f\nout\n"));   /* A function's own */
    CHECK(sh_prints("set -- a b c; shift; echo $*", "b c\n"));
    /* What a function's "set --" makes is freed when it returns: 2000 calls
     * leave the shell's heap as it was (the leak printed nothing at all) */
    CHECK(sh_prints("f() { set -- one two three four five six seven eight; }; f; "
                    "a=$(grep Size /proc/$$/status | cut -f 2); i=0; "
                    "while [ $i -lt 2000 ]; do f; i=$((i + 1)); done; "
                    "b=$(grep Size /proc/$$/status | cut -f 2); [ -n \"$a\" ] && [ \"$a\" = \"$b\" ] && echo same", "same\n"));
    CHECK(sh_prints("set -x -- p q; set +x; echo $1 $#", "p 2\n"));                     /* Options and arguments at once */

    /* getopts: grouped letters, an argument joined and apart, the end at a
     * word that is not an option, and at --. (The loops are bounded: a
     * getopts that never moves on must fail them, not hang them.) */
    CHECK(sh_prints("set -- -ab val -cfoo rest; n=0; while [ $n -lt 9 ] && getopts ab:c: o; do n=$((n + 1)); echo $o [$OPTARG]; done; "
                    "shift $((OPTIND - 1)); echo left $*",
                    "a []\nb [val]\nc [foo]\nleft rest\n"));
    CHECK(sh_prints("set -- -a -- -b; n=0; while [ $n -lt 9 ] && getopts ab o; do n=$((n + 1)); echo $o; done; echo $OPTIND", "a\n3\n"));
    CHECK(sh_prints("set -- -x; getopts ab o; echo $? $o", "0 ?\n"));                     /* Not in the list */
    CHECK(sh_prints("set -- -x; getopts :ab o; echo $o $OPTARG", "? x\n"));               /* Silent */
    CHECK(sh_prints("set -- -b; getopts :ab: o; echo $o $OPTARG", ": b\n"));              /* Silent, argument missing */
    CHECK(sh_prints("getopts a o -a; echo $? $o", "0 a\n"));                           /* From the words given */
    CHECK(sh_prints("set -- plain; getopts a o; echo $? $OPTIND", "1 1\n"));
    CHECK(sh_ends("getopts", "", 2));
}

/* set -e and each of its exceptions, set -u, set -x, '!', shift, local,
 * $$ and trap */
static void check_shell_options(void) {
    /* set -e: the first command that fails ends the shell, with its status */
    CHECK(sh_ends("set -e; echo a; false; echo b", "a\n", 1));
    CHECK(sh_ends("set -e; echo a; sh -c 'exit 3'; echo b", "a\n", 3));
    CHECK(sh_ends("set -e; for i in 1 2; do echo $i; false; done; echo no", "1\n", 1));
    CHECK(sh_ends("set -e; f() { return 4; }; f; echo no", "", 4));
    CHECK(sh_ends("set -e; true && false; echo no", "", 1));               /* The last of an && list counts */
    CHECK(sh_prints("set -e; { false; } | cat; echo yes", "yes\n"));        /* A pipeline's status is its last command's */
    /* ... except in a condition: if, elif, while, until */
    CHECK(sh_prints("set -e; if false; then echo no; fi; echo after-if", "after-if\n"));
    CHECK(sh_prints("set -e; if false; then :; elif false; then :; else echo else; fi", "else\n"));
    CHECK(sh_prints("set -e; while false; do :; done; echo after-while", "after-while\n"));
    CHECK(sh_prints("set -e; n=0; until [ $n -eq 2 ]; do n=$((n + 1)); done; echo n=$n", "n=2\n"));
    /* ... before the end of an && or || list */
    CHECK(sh_prints("set -e; false && echo no; echo after-and", "after-and\n"));
    CHECK(sh_prints("set -e; false || echo or-ran; echo after-or", "or-ran\nafter-or\n"));
    /* ... after '!' */
    CHECK(sh_prints("set -e; ! true; echo after-not", "after-not\n"));
    /* A function called in a condition runs with set -e set aside */
    CHECK(sh_prints("set -e; f() { false; echo in-f; }; if f; then echo t; fi", "in-f\nt\n"));
    CHECK(sh_prints("set -e; set +e; false; echo still", "still\n"));

    /* ! turns a status round, for a compound command too */
    CHECK(sh_prints("! false; echo $?; ! true; echo $?; ! { false; }; echo $?", "0\n1\n0\n"));
    CHECK(sh_ends("!", "", 2));

    /* set -u: an unset variable ends the shell (not at the terminal) */
    CHECK(sh_ends("set -u; echo $nothing; echo no", "", 2));
    CHECK(sh_ends("set -u; f() { echo $1; }; f", "", 2));
    CHECK(sh_ends("set -u; echo ${nothing}x", "", 2));
    CHECK(sh_prints("set -u; x=1; echo $x $# $?", "1 0 0\n"));
    CHECK(sh_prints("set -u; set +u; echo [$nothing]", "[]\n"));

    /* set -x: each command, expanded, after "+ " on standard error */
    CHECK(sh_prints("{ set -x; x=1; echo $x \"a b\"; } 2>&1", "+ x=1\n+ echo 1 a b\n1 a b\n"));
    CHECK(sh_ends("set -q", "", 2));

    /* shift */
    CHECK(sh_prints("f() { shift; echo $# $1; shift 2; echo $# $1; }; f a b c d", "3 b\n1 d\n"));
    /* (Bounded: a shift that did nothing must fail the check, not hang it) */
    CHECK(sh_prints("f() { n=0; while [ $# -gt 0 ] && [ $n -lt 3 ]; do echo $1; shift; n=$((n + 1)); done; }; f x 'y z'", "x\ny z\n"));
    CHECK(sh_ends("f() { shift 3; }; f a b", "", 1));

    /* local: the function's own, the old value back when it returns */
    CHECK(sh_prints("x=outer; f() { local x=inner; echo $x; }; f; echo $x", "inner\nouter\n"));
    CHECK(sh_prints("f() { local y=1; }; f; echo [$y]", "[]\n"));
    CHECK(sh_prints("x=a; g() { echo g:$x; x=c; }; f() { local x; x=b; g; echo f:$x; }; f; echo $x", "g:b\nf:c\na\n"));
    CHECK(sh_prints("export E=env; f() { local E=mine; sh -c 'echo $E'; }; f; sh -c 'echo $E'", "mine\nenv\n"));
    CHECK(sh_ends("local x=1", "", 1));                                      /* Not in a function */

    /* $$: the shell's process, also in a $(...); another shell has its own */
    CHECK(sh_prints("[ $$ -gt 1 ] && [ $$ = $(echo $$) ] && [ $$ != $(sh -c 'echo $$') ] && echo ok", "ok\n"));

    /* trap */
    CHECK(sh_prints("trap 'echo bye' EXIT; echo hi", "hi\nbye\n"));
    CHECK(sh_ends("trap 'echo bye $?' EXIT; sh -c 'exit 5'", "bye 5\n", 5));
    CHECK(sh_ends("trap 'exit 7' EXIT; true", "", 7));
    CHECK(sh_ends("trap 'echo bye' EXIT; set -u; echo $nothing", "bye\n", 2));
    CHECK(sh_prints("trap 'echo got USR1' USR1; kill -10 $$; echo after", "got USR1\nafter\n"));
    CHECK(sh_prints("trap 'false' USR1; kill -10 $$; echo $?", "0\n"));      /* $? as it was before the trap */
    CHECK(sh_prints("trap 'echo got' TERM; kill -15 $$; kill -15 $$; trap '' TERM; kill -15 $$; echo end", "got\ngot\nend\n"));
    CHECK(sh_ends("trap 'echo t' TERM; trap - TERM; kill -15 $$; echo no", "", 128 + SIGTERM));
    CHECK(sh_ends("trap 'echo t' 15; trap 15; kill -15 $$; echo no", "", 128 + SIGTERM));
    CHECK(sh_prints("trap 'echo a' INT; trap \"it's\" SIGTERM; trap", "trap -- 'echo a' INT\ntrap -- 'it'\\''s' TERM\n"));
    CHECK(sh_ends("trap 'echo x' NOPE", "", 1));
    CHECK(sh_ends("trap 'echo x' KILL", "", 1));
    /* A child running shell code is a subshell: no EXIT trap of its own */
    CHECK(sh_prints("trap 'echo bye' EXIT; set -u; { echo $nothing; } | cat; echo after", "after\nbye\n"));
}

/* The shell's if, while and for, break and continue, and test / [ */
static void check_shell_control(void) {
    /* if: the branch taken; elif; nothing taken is status 0; the status of the branch */
    CHECK(sh_prints("if false; then echo a; else echo b; fi", "b\n"));
    CHECK(sh_prints("if true; then echo a; else echo b; fi", "a\n"));
    CHECK(sh_prints("if false; then echo a; elif true; then echo b; else echo c; fi", "b\n"));
    CHECK(sh_prints("if false; then echo a; elif false; then echo b; else echo c; fi", "c\n"));
    CHECK(sh_prints("if false; then echo a; fi; echo s=$?", "s=0\n"));
    CHECK(sh_ends("if true; then false; fi", "", 1));
    CHECK(sh_prints("if true && false || true; then echo list; fi", "list\n"));
    CHECK(sh_prints("if true; then echo a; fi && echo after", "a\nafter\n"));

    /* Loops: the words expanded once; nesting; break and continue, with N */
    CHECK(sh_prints("for x in 1 2 3; do echo n$x; done", "n1\nn2\nn3\n"));
    CHECK(sh_prints("for x in; do echo never; done; echo s=$?", "s=0\n"));
    CHECK(sh_prints("for f in $(echo p q) 'r s'; do echo [$f]; done", "[p]\n[q]\n[r s]\n"));
    CHECK(sh_prints("export I=x; while [ \"$I\" != xxx ]; do export I=\"$I\"x; echo $I; done", "xx\nxxx\n"));
    CHECK(sh_prints("while false; do echo never; done; echo s=$?", "s=0\n"));
    CHECK(sh_prints("export I=x; until [ \"$I\" = xxx ]; do export I=\"$I\"x; echo $I; done", "xx\nxxx\n"));
    CHECK(sh_prints("until true; do echo never; done; echo s=$?", "s=0\n"));
    CHECK(sh_prints("until false; do until false; do break 2; done; done; echo out", "out\n"));
    CHECK(sh_ends("until false; do; done", "", 2));
    CHECK(sh_prints("for x in a b c d; do if [ $x = b ]; then continue; fi; if [ $x = d ]; then break; fi; echo $x; done",
                    "a\nc\n"));
    CHECK(sh_prints("for i in 1 2; do for j in a b c; do if [ $j = b ]; then continue 2; fi; echo $i$j; done; done",
                    "1a\n2a\n"));
    CHECK(sh_prints("for i in 1 2; do for j in a b; do break 2; done; echo never; done; echo out", "out\n"));
    CHECK(sh_prints("while true; do while true; do break 5; done; done; echo out", "out\n"));   /* N past the outermost */
    CHECK(sh_ends("break", "", 1));                                       /* Not in a loop */

    /* test and [ */
    CHECK(sh_prints("[ -f /etc/motd ] && [ -d /bin ] && [ -e /bin/sh ] && echo yes", "yes\n"));
    CHECK(sh_ends("[ -d /etc/motd ]", "", 1));
    CHECK(sh_ends("[ -e /no/such ]", "", 1));
    CHECK(sh_prints("[ -z '' ] && [ -n x ] && [ abc ] && test a = a && [ a != b ] && echo yes", "yes\n"));
    CHECK(sh_ends("[ '' ]", "", 1));
    CHECK(sh_ends("test", "", 1));
    CHECK(sh_prints("[ 3 -lt 10 ] && [ 10 -gt 3 ] && [ -2 -le -2 ] && [ 5 -ge 5 ] && [ 1 -ne 2 ] && [ 7 -eq 7 ] && echo yes",
                    "yes\n"));
    CHECK(sh_ends("[ 10 -lt 3 ]", "", 1));
    CHECK(sh_prints("[ ! -e /no/such ] && [ ! a = b ] && echo yes", "yes\n"));
    CHECK(sh_ends("[ ! a = a ]", "", 1));
    CHECK(sh_ends("[ 1 -eq x ]", "", 2));                                /* Not a number */
    CHECK(sh_ends("[ a = a", "", 2));                                    /* No ] */
    CHECK(sh_ends("[ a b c d ]", "", 2));

    /* Syntax errors, and text that stops inside a command */
    CHECK(sh_ends("fi", "", 2));
    CHECK(sh_ends("if true; then echo x", "", 2));
    CHECK(sh_ends("if true; echo x; fi", "", 2));                       /* No then */
    CHECK(sh_ends("while true; do; done", "", 2));
    CHECK(sh_ends("if; then echo x; fi", "", 2));
    CHECK(sh_ends("for 1 in a; do echo x; done", "", 2));
    CHECK(sh_ends("echo a && then", "", 2));
    CHECK(sh_ends("if true; then echo x; fi extra", "", 2));

    /* A script over several lines, with comments and nesting */
    long fd = open("/tmp/control.sh", O_WRONLY | O_CREAT | O_TRUNC);
    static const char script[] =
        "# Counts down, then says which numbers were even\n"
        "for n in 3 2 1\n"
        "do\n"
        "    if [ $n = 2 ]   # the even one\n"
        "    then\n"
        "        echo even $n\n"
        "    else\n"
        "        echo odd $n\n"
        "    fi\n"
        "done\n"
        "echo done\n";
    CHECK(fd >= 0 && write((int)fd, script, sizeof(script) - 1) == (long)sizeof(script) - 1);
    close((int)fd);
    CHECK(sh_prints("sh /tmp/control.sh", "odd 3\neven 2\nodd 1\ndone\n"));
    fd = open("/tmp/control.sh", O_WRONLY | O_TRUNC);
    static const char unfinished[] = "if true\nthen\necho unfinished\n";
    CHECK(fd >= 0 && write((int)fd, unfinished, strlen(unfinished)) == (long)strlen(unfinished));
    close((int)fd);
    CHECK(sh_ends("sh /tmp/control.sh", "", 2));                         /* The file ends inside the if */
    CHECK(unlink("/tmp/control.sh") == 0);
}

/* Feed every byte of `keys` to the editor; the result of the last one */
static int type_keys(lineedit_t *e, const char *keys) {
    int result = LINEEDIT_MORE;
    for (; *keys; keys++) result = lineedit_key(e, *keys);
    return result;
}

static int completion_asked_command;      /* What complete_fixed was last asked */

/* A completion function with a fixed set of names, for the editor's tests:
 * programs "cat", "cd" and "cmp"; in any directory "notes.txt", "notes.md"
 * and "src/". Records whether it was asked for a program name. */
static void complete_fixed(lineedit_t *e, const char *word, int command) {
    static const char *const programs[] = { "cmp", "cat", "cd" };
    static const char *const files[] = { "notes.txt", "src/", "notes.md" };
    const char *start = word;
    for (const char *c = word; *c; c++) if (*c == '/') start = c + 1;
    completion_asked_command = command;
    const char *const *names = command && start == word ? programs : files;
    for (int i = 0; i < 3; i++) {
        if (strncmp(names[i], start, strlen(start)) == 0) lineedit_add_match(e, names[i]);
    }
}

/* The shell's line editor, without a terminal: keys in, line and cursor out */
static void check_line_editing(void) {
    /* isatty: the console is the terminal, a pipe and a file are not */
    int p[2];
    CHECK(pipe(p) == 0 && isatty(p[0]) == 0 && isatty(p[1]) == 0);
    close(p[0]); close(p[1]);
    long fd = open("/dev/console", O_RDONLY);
    CHECK(fd >= 0 && isatty((int)fd) == 1);
    close((int)fd);
    fd = open("/etc/motd", O_RDONLY);
    CHECK(fd >= 0 && isatty((int)fd) == 0);
    close((int)fd);
    CHECK(isatty(99) == -EBADF);

    static lineedit_t e;
    lineedit_start(&e);
    CHECK(type_keys(&e, "helo") == LINEEDIT_MORE && strcmp(e.line, "helo") == 0 && e.cursor == 4);
    type_keys(&e, "\033[D\033[Dl");                    /* Left twice, insert */
    CHECK(strcmp(e.line, "hello") == 0 && e.cursor == 3);
    type_keys(&e, "\033[H\033[3~");                     /* Home, Delete */
    CHECK(strcmp(e.line, "ello") == 0 && e.cursor == 0);
    type_keys(&e, "\x05!\x7f\x7f");                    /* Ctrl-E, insert, Backspace twice */
    CHECK(strcmp(e.line, "ell") == 0 && e.cursor == 3);
    type_keys(&e, " one two\x17");                      /* Ctrl-W: the word before the cursor */
    CHECK(strcmp(e.line, "ell one ") == 0);
    type_keys(&e, "\x01\033[C\x0b");                   /* Ctrl-A, Right, Ctrl-K */
    CHECK(strcmp(e.line, "e") == 0 && e.cursor == 1);
    type_keys(&e, "cho a\x02\t");                      /* Other control keys do nothing */
    CHECK(strcmp(e.line, "echo a") == 0);
    CHECK(lineedit_key(&e, '\r') == LINEEDIT_DONE);

    /* History: up and down through the lines entered, the line being typed kept */
    lineedit_start(&e);
    CHECK(type_keys(&e, "echo b\r") == LINEEDIT_DONE);
    lineedit_start(&e);
    CHECK(type_keys(&e, "\r") == LINEEDIT_DONE);         /* An empty line is not remembered */
    lineedit_start(&e);
    type_keys(&e, "unfinished\033[A");
    CHECK(strcmp(e.line, "echo b") == 0 && e.cursor == 6);
    type_keys(&e, "\033[A");
    CHECK(strcmp(e.line, "echo a") == 0);
    type_keys(&e, "\033[A");                              /* Nothing older: stays */
    CHECK(strcmp(e.line, "echo a") == 0);
    type_keys(&e, "\033[B\033[B");
    CHECK(strcmp(e.line, "unfinished") == 0);
    CHECK(type_keys(&e, "\x15\x04") == LINEEDIT_EOF);   /* Ctrl-U, then Ctrl-D on the empty line */

    /* A full line takes no more, and the history keeps the newest lines */
    lineedit_start(&e);
    for (int i = 0; i < LINEEDIT_MAX + 10; i++) lineedit_key(&e, 'x');
    CHECK(e.len == LINEEDIT_MAX - 1);
    for (int i = 0; i < LINEEDIT_HISTORY + 5; i++) {
        lineedit_start(&e);
        char line[8] = { 'n', (char)('0' + i / 10), (char)('0' + i % 10), '\r', 0 };
        type_keys(&e, line);
    }
    CHECK(e.count == LINEEDIT_HISTORY && strcmp(e.history[LINEEDIT_HISTORY - 1], "n68") == 0 &&
          strcmp(e.history[0], "n05") == 0);

    /* Tab: nothing without a completion function */
    lineedit_start(&e);
    type_keys(&e, "ca\t");
    CHECK(strcmp(e.line, "ca") == 0);
    /* One match: completed, with a space; a program name first, a file after */
    e.complete = complete_fixed;
    type_keys(&e, "\t");
    CHECK(strcmp(e.line, "cat ") == 0 && completion_asked_command == 1);
    type_keys(&e, "s\t");
    CHECK(strcmp(e.line, "cat src/") == 0 && completion_asked_command == 0);   /* A directory: no space */
    /* Several: as far as they agree; a second Tab asks for the sorted list */
    type_keys(&e, "\x15" "cat n\t");
    CHECK(strcmp(e.line, "cat notes.") == 0 && e.show_matches == 0 && e.match_count == 2);
    type_keys(&e, "\t");
    CHECK(e.show_matches == 1 && strcmp(e.matches[0], "notes.md") == 0 && strcmp(e.matches[1], "notes.txt") == 0);
    e.show_matches = 0;
    type_keys(&e, "t\t");
    CHECK(strcmp(e.line, "cat notes.txt ") == 0);
    /* After '|' or ';' a program name goes again; the cursor need not be at the end */
    lineedit_start(&e);
    type_keys(&e, "ls | c\t");
    CHECK(strcmp(e.line, "ls | c") == 0 && completion_asked_command == 1 && e.match_count == 3);
    type_keys(&e, "a\033[Hsrc/no\t");                                     /* Before "ls" */
    CHECK(strcmp(e.line, "src/notes.ls | ca") == 0 && completion_asked_command == 1);
    e.complete = NULL;
}

/* The shell's wildcard matcher */
static void check_wildcards(void) {
    CHECK(glob_match("*", "anything") && glob_match("*", "") && !glob_match("*", ".hidden"));
    CHECK(glob_match(".*", ".hidden") && glob_match("*.c", "sh.c") && !glob_match("*.c", "sh.h"));
    CHECK(glob_match("a*b*c", "aXbYbZc") && !glob_match("a*b*c", "aXbYbZ"));
    CHECK(glob_match("?", "x") && !glob_match("?", "") && !glob_match("?", "xy"));
    CHECK(glob_match("f??e", "fake") && !glob_match("f??e", "fame!"));
    CHECK(glob_match("[abc]x", "bx") && !glob_match("[abc]x", "dx"));
    CHECK(glob_match("[a-c][0-9]", "b7") && !glob_match("[a-c][0-9]", "d7"));
    CHECK(glob_match("[!a-c]*", "dog") && !glob_match("[!a-c]*", "cat"));
    CHECK(glob_match("[]]", "]") && glob_match("[a-]", "-"));    /* ']' first, '-' last: members */
    CHECK(glob_match("[ab", "[ab") && !glob_match("[ab", "a"));   /* Unclosed: literal */
    CHECK(glob_match("plain", "plain") && !glob_match("plain", "plainer"));
    /* Many stars against a long name that does not match: answered quickly */
    CHECK(!glob_match("*a*a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    CHECK(glob_has_wildcard("x*") && glob_has_wildcard("[a]") && !glob_has_wildcard("plain"));
}

/* Read a whole small file into buf[size] (NUL-terminated); its length, or
 * a negative error */
static long read_small(const char *path, char *buf, size_t size) {
    long fd = open(path, O_RDONLY);
    if (fd < 0) return fd;
    long total = 0, n;
    while (total < (long)size - 1 && (n = read((int)fd, buf + total, size - 1 - (size_t)total)) > 0) total += n;
    close((int)fd);
    if (n < 0 && total == 0) return n;
    buf[total] = '\0';
    return total;
}

/* The number after "key:\t" in a /proc status text, or -1 */
static long status_number(const char *text, const char *key) {
    size_t k = strlen(key);
    for (const char *p = text; *p; p++) {
        if ((p == text || p[-1] == '\n') && strncmp(p, key, k) == 0 && p[k] == ':' && p[k + 1] == '\t') {
            return p[k + 2] >= '0' && p[k + 2] <= '9' ? atoi(p + k + 2) : -1;
        }
    }
    return -1;
}

/* Whether `text` contains `part` */
static int has_text(const char *text, const char *part) {
    size_t n = strlen(part);
    for (; *text; text++) if (strncmp(text, part, n) == 0) return 1;
    return 0;
}

/* /proc/interrupts: a column per CPU, and each disk either on its own
 * MSI-X vector or on a line (which one is the kernel's choice, checked by
 * the boot log; here only that the table says one of them) */
static void check_proc_interrupts(void) {
    static char text[2048];
    CHECK(read_small("/proc/interrupts", text, sizeof(text)) > 0);
    CHECK(has_text(text, "CPU0"));
    CHECK(has_text(text, "  MSI-X virtio-blk 0\n") || has_text(text, "  line\n"));
}

/* /proc/<pid>: this process's own status through /proc/self, a child's
 * exact arguments once it has exec'd, ENOENT once it has been reaped */
static void check_proc(void) {
    static char text[1024];
    CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
    CHECK(status_number(text, "Pid") == getpid() && status_number(text, "Uid") == (long)getuid());
    CHECK(has_text(text, "Name:\tsyscheck\n") && has_text(text, "Kind:\tuser\n"));

    /* Size: the program, heap and mappings (what /bin/top shows); a mapping
     * counts in full at once, touched or not, and goes when unmapped */
    long size_before = status_number(text, "Size");
    CHECK(size_before > status_number(text, "Heap") + status_number(text, "Mapped"));  /* The program too */
    void *block = mmap_anon(256 * 1024);
    CHECK(block != NULL);
    CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
    CHECK(status_number(text, "Size") == size_before + 256);
    CHECK(block && munmap(block, 256 * 1024) == 0);
    CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
    CHECK(status_number(text, "Size") == size_before);

    /* Read in 7-byte pieces, a status is one moment: a mapping made after
     * the first piece is not in it, and its longer numbers do not shift
     * the lines after them; read again from the start, it is */
    long fd = open("/proc/self/status", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        long total = read((int)fd, text, 7), n7;
        CHECK(total == 7);
        void *big = mmap_anon(8 * 1024 * 1024);               /* Untouched: costs no frames */
        CHECK(big != NULL);
        while (total > 0 && total < (long)sizeof(text) - 1 &&
               (n7 = read((int)fd, text + total, (size_t)total + 7 < sizeof(text) ? 7 : sizeof(text) - 1 - (size_t)total)) > 0) {
            total += n7;
        }
        text[total > 0 ? total : 0] = '\0';
        int lines = 0;
        for (const char *c = text; *c; c++) lines += *c == '\n';
        CHECK(status_number(text, "Size") == size_before && status_number(text, "Pid") == getpid());
        CHECK(lines == 21);
        CHECK(lseek((int)fd, 0, SEEK_SET) == 0);
        total = read((int)fd, text, sizeof(text) - 1);
        text[total > 0 ? total : 0] = '\0';
        CHECK(status_number(text, "Size") == size_before + 8192);
        close((int)fd);
        CHECK(big && munmap(big, 8 * 1024 * 1024) == 0);
    }

    /* Resident: what is present. 64 pages touched add exactly 256 kB and go
     * again with munmap (the second round counts: in the first, code pages
     * of the calls themselves are read in too, page by page) */
    for (int round = 0; round < 2; round++) {
        CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
        long resident_before = status_number(text, "Resident");
        char *pages = mmap_anon(64 * 4096);
        CHECK(pages != NULL);
        if (!pages) break;
        for (int i = 0; i < 64; i++) pages[i * 4096] = 1;
        CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
        long resident_touched = status_number(text, "Resident");
        CHECK(munmap(pages, 64 * 4096) == 0);
        CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
        long resident_after = status_number(text, "Resident");
        if (round == 1) CHECK(resident_touched == resident_before + 256 && resident_after == resident_before);
    }
    /* Shared: after fork, this process's pages are the child's too */
    CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
    long shared_before = status_number(text, "Shared");
    int hold[2];
    CHECK(pipe(hold) == 0);
    long child = fork();
    if (child == 0) {
        char byte;
        close(hold[1]);
        read(hold[0], &byte, 1);
        exit(0);
    }
    close(hold[0]);
    CHECK(read_small("/proc/self/status", text, sizeof(text)) > 0);
    CHECK(status_number(text, "Shared") > shared_before + 64);
    close(hold[1]);
    int child_status = -1;
    CHECK(waitpid(child, &child_status, 0) == child && child_status == 0);

    char target[16];
    long n = readlink("/proc/self", target, sizeof(target) - 1);
    CHECK(n > 0 && n < (long)sizeof(target));
    target[n > 0 ? n : 0] = '\0';
    CHECK(atoi(target) == getpid());

    long pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/spin", "sleep", "and two", "more", NULL };
        execv("/bin/spin", args);
        exit(1);
    }
    CHECK(pid > 0);
    char path[40] = "/proc/";
    {
        char digits[16];
        int d = 0;
        long v = pid;
        do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        size_t at = 6;
        while (d > 0) path[at++] = digits[--d];
        memcpy(path + at, "/cmdline", 9);
    }
    static const char expected[] = "/bin/spin\0sleep\0and two\0more";   /* And a final NUL */
    int matched = 0;
    for (int tries = 0; tries < 200 && !matched; tries++) {            /* Until the child has exec'd */
        n = read_small(path, text, sizeof(text));
        matched = n == (long)sizeof(expected) && memcmp(text, expected, sizeof(expected)) == 0;
        if (!matched) sleep_ms(10);
    }
    CHECK(matched);
    CHECK(kill(pid, SIGKILL) == 0);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(read_small(path, text, sizeof(text)) == -ENOENT);            /* Reaped: gone */
    CHECK(read_small("/proc/999999/status", text, sizeof(text)) == -ENOENT);
    CHECK(unlink("/proc/1/status") == -EPERM);                         /* Not even root's to remove */
}

/* "/proc/self/fd/N" into path[24] */
static void self_fd_path(char path[24], int fd) {
    memcpy(path, "/proc/self/fd/", 14);
    size_t at = 14;
    if (fd >= 10) path[at++] = (char)('0' + fd / 10);
    path[at++] = (char)('0' + fd % 10);
    path[at] = '\0';
}

/* Whether the directory `path` lists an entry called `name` */
static int dir_lists(const char *path, const char *name) {
    long fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct dirent entry;
    int found = 0;
    while (!found && readdir((int)fd, &entry) == 1) found = strcmp(entry.name, name) == 0;
    close((int)fd);
    return found;
}

/* /proc/<pid>/maps and /proc/<pid>/fd: this process's own after an
 * anonymous and a file mapping, a pipe and an open file; each goes again
 * when it is unmapped or closed; and they are the owner's alone */
static void check_proc_maps_fd(void) {
    static char text[4096];
    CHECK(read_small("/proc/self/maps", text, sizeof(text)) > 0);
    CHECK(has_text(text, " r-x program syscheck\n") && has_text(text, " rw- stack\n"));

    char *anon = mmap_anon(3 * 4096);
    long motd = open("/etc/motd", O_RDONLY);
    char *mapped = motd >= 0 ? mmap_file((int)motd, 4096, 0, PROT_READ) : NULL;
    CHECK(anon != NULL && mapped != NULL);
    CHECK(read_small("/proc/self/maps", text, sizeof(text)) > 0);
    CHECK(has_text(text, " rw- anonymous\n"));
    CHECK(has_text(text, " r-- file /etc/motd +0\n"));
    /* The anonymous line starts at the address mmap returned, 3 pages long */
    {
        static const char hex[] = "0123456789abcdef";
        char line[40];
        unsigned long a = (unsigned long)anon, b = a + 3 * 4096;
        for (int i = 0; i < 12; i++) {
            line[11 - i] = hex[(a >> (4 * i)) & 15];
            line[24 - i] = hex[(b >> (4 * i)) & 15];
        }
        line[12] = '-';
        memcpy(line + 25, " rw- anonymous", 15);
        CHECK(has_text(text, line));
    }

    int ends[2];
    CHECK(pipe(ends) == 0);
    char path[24];
    self_fd_path(path, (int)motd);
    CHECK(read_small(path, text, sizeof(text)) > 0 && strcmp(text, "/etc/motd\n") == 0);
    self_fd_path(path, ends[0]);
    CHECK(read_small(path, text, sizeof(text)) > 0 && strcmp(text, "pipe (read end)\n") == 0);
    self_fd_path(path, ends[1]);
    CHECK(read_small(path, text, sizeof(text)) > 0 && strcmp(text, "pipe (write end)\n") == 0);
    CHECK(read_small("/proc/self/fd/0", text, sizeof(text)) > 0 && strcmp(text, "console\n") == 0);
    /* Descriptor 12 (listing the directory opens the lowest free one) */
    CHECK(dup2(ends[1], 12) == 12 && dir_lists("/proc/self/fd", "12"));
    CHECK(read_small("/proc/self/fd/12", text, sizeof(text)) > 0 && strcmp(text, "pipe (write end)\n") == 0);
    close(12);
    CHECK(read_small("/proc/self/fd/12", text, sizeof(text)) == -ENOENT);   /* Closed: gone */
    CHECK(!dir_lists("/proc/self/fd", "12"));
    close(ends[1]);
    close(ends[0]);

    CHECK(munmap(anon, 3 * 4096) == 0 && munmap(mapped, 4096) == 0);
    close((int)motd);
    CHECK(read_small("/proc/self/maps", text, sizeof(text)) > 0);
    CHECK(!has_text(text, "anonymous") && !has_text(text, "/etc/motd"));

    /* They name files: bob may read his own, not root's */
    long pid = fork();
    if (pid == 0) {
        if (setgid(100) != 0 || setuid(1001) != 0) exit(9);
        if (read_small("/proc/1/maps", text, sizeof(text)) != -EACCES) exit(1);
        if (open("/proc/1/fd", O_RDONLY) != -EACCES) exit(2);
        if (read_small("/proc/1/fd/0", text, sizeof(text)) != -EACCES) exit(3);
        if (read_small("/proc/1/status", text, sizeof(text)) <= 0) exit(4);      /* status stays public */
        if (read_small("/proc/self/maps", text, sizeof(text)) <= 0) exit(5);
        if (read_small("/proc/self/fd/1", text, sizeof(text)) <= 0) exit(6);
        exit(0);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && status == 0);
}

/* Pipes: data, end of file, a closed reader (EPIPE), a full pipe */
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
    CHECK(waitpid(pid, &status, 0) == pid);
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
    waitpid(pid, &status, 0);
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
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + 9);
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

/* How long until read() on `fd` reports end-of-file, in milliseconds */
static unsigned long ms_until_eof(int fd) {
    char byte;
    unsigned long start = clock_ns();
    while (read(fd, &byte, 1) > 0) { }
    return (clock_ns() - start) / 1000000UL;
}

/* A child that execs a program which runs for a second, holding whatever
 * descriptors survived the exec. Returns how long the read end of a pipe
 * takes to see end-of-file (-1 if the child failed), and in *alive_at_eof
 * whether the program was still running then: it is if the child's copy
 * of the write end was closed by the exec, and not if it was kept. (That,
 * rather than a time limit, says "at once": a slow machine can take long
 * to start a program.) */
static long exec_holding_pipe(int flags, const char *seconds, int *alive_at_eof) {
    int p[2];
    *alive_at_eof = 0;
    if (pipe2(p, flags) != 0) return -1;
    long pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/sleep", (char *)seconds, NULL };
        execve("/bin/sleep", args, NULL);
        exit(99);
    }
    close(p[1]);
    unsigned long ms = ms_until_eof(p[0]);
    /* Alive if seen in a system call (exec, or its sleep) after the end of
     * file: a dying process is in none from before its files close, and
     * makes no new one. Not being in one proves nothing: a live program
     * also runs its own code and takes page faults loading itself (one
     * look, on a slow machine, landed there). So look until it is seen in
     * a call or can be reaped; one of the two always comes. */
    int status = -1, reaped = 0;
    while (!*alive_at_eof && !reaped) {
        if (syscall_of(pid) >= 0) *alive_at_eof = 1;
        else if (waitpid(pid, &status, WNOHANG) == pid) reaped = 1;
        else sleep_ms(1);
    }
    close(p[0]);
    if (*alive_at_eof) kill(pid, SIGKILL);      /* Seen alive: no need to wait out its sleep */
    if (!reaped) waitpid(pid, &status, 0);
    return status == 0 || (*alive_at_eof && status == 128 + SIGKILL) ? (long)ms : -1;
}

/* dup, dup2, close-on-exec, and waitpid's options and errors */
static void check_descriptors_and_wait(void) {
    char buf[8];
    int p[2], status;

    /* dup: a second descriptor for the same open file */
    CHECK(pipe(p) == 0);
    long copy = dup(p[1]);
    CHECK(copy > p[1]);
    CHECK(close(p[1]) == 0);
    CHECK(write((int)copy, "ab", 2) == 2);                /* Still open through the copy */
    CHECK(read(p[0], buf, sizeof(buf)) == 2 && buf[0] == 'a' && buf[1] == 'b');
    CHECK(close((int)copy) == 0);
    CHECK(read(p[0], buf, sizeof(buf)) == 0);             /* Now every writer has gone */
    CHECK(dup(99) == -EBADF && dup((int)copy) == -EBADF);

    /* The close-on-exec flag belongs to a descriptor, not to the file */
    CHECK(fcntl(p[0], F_GETFD, 0) == 0);
    CHECK(fcntl(p[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(p[0], F_GETFD, 0) == FD_CLOEXEC);
    copy = dup(p[0]);
    CHECK(copy >= 0 && fcntl((int)copy, F_GETFD, 0) == 0);
    CHECK(fcntl(p[0], F_SETFD, 0) == 0 && fcntl(p[0], F_GETFD, 0) == 0);
    CHECK(fcntl(p[0], 77, 0) == -EINVAL && fcntl(99, F_GETFD, 0) == -EBADF);
    close((int)copy);
    close(p[0]);

    CHECK(pipe2(p, O_CLOEXEC) == 0);
    CHECK(fcntl(p[0], F_GETFD, 0) == FD_CLOEXEC && fcntl(p[1], F_GETFD, 0) == FD_CLOEXEC);
    close(p[0]);
    close(p[1]);
    CHECK(pipe2(p, 0x1234) == -EINVAL);
    CHECK(pipe2((int *)0xffffffff80000000UL, 0) == -EFAULT);

    long fd = open("/bin/hello", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && fcntl((int)fd, F_GETFD, 0) == FD_CLOEXEC);
    CHECK(close((int)fd) == 0);
    fd = open("/bin/hello", O_RDONLY);                    /* The slot is reused: the flag was not left behind */
    CHECK(fd >= 0 && fcntl((int)fd, F_GETFD, 0) == 0);
    close((int)fd);

    /* What the flag does: the descriptor does not survive an exec */
    int kept_alive = 0, closed_alive = 0;
    long kept = exec_holding_pipe(0, "1", &kept_alive);
    long closed = exec_holding_pipe(O_CLOEXEC, "5", &closed_alive);  /* Killed once seen: no wait */
    CHECK(kept >= 800 && !kept_alive);                    /* Held open by the new program for its whole second */
    CHECK(closed >= 0 && closed_alive);                   /* Gone as soon as it started, while it still ran */

    /* waitpid that does not wait */
    while (waitpid(-1, &status, WNOHANG) > 0) { }
    CHECK(waitpid(-1, &status, WNOHANG) == -ECHILD);      /* Nobody to wait for */
    int go[2];
    CHECK(pipe(go) == 0);
    long pid = fork();
    if (pid == 0) {
        close(go[1]);
        char byte;
        read(go[0], &byte, 1);                            /* Alive until told otherwise */
        exit(7);
    }
    close(go[0]);
    status = -1;
    CHECK(waitpid(pid, &status, WNOHANG) == 0 && status == -1); /* Alive: 0, and status untouched */
    CHECK(waitpid(-1, &status, WNOHANG) == 0);
    CHECK(waitpid(pid, &status, 0x40) == -EINVAL);
    CHECK(waitpid(pid, &status, WNOHANG) == 0);          /* Asking again does not wait either */
    CHECK(write(go[1], "x", 1) == 1);
    close(go[1]);
    int polls = 0;
    long got = 0;
    while ((got = waitpid(pid, &status, WNOHANG)) == 0 && polls < 500) {
        sleep_ms(10);
        polls++;
    }
    CHECK(got == pid && status == 7);
    CHECK(waitpid(pid, &status, WNOHANG) == -ECHILD);
}

/* The stack grows as it is used, in the parent and in a child */
static void check_stack_growth(void) {
    /* The stack starts at 16 KiB and grows as it is used: 2 MiB works */
    CHECK(recurse(500) == 500);

    /* A child inherits the grown stack and can grow its own further */
    long pid = fork();
    if (pid == 0) exit(recurse(1000) == 1000 ? 0 : 1);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);

    /* Runaway recursion hits the limit (8 MiB) and is killed, not the system */
    pid = fork();
    if (pid == 0) exit((int)recurse(1 << 20));
    status = -1;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(status == 128 + 11);

    /* The kernel grows the stack for itself too: a buffer in a part of it
     * that has never been touched is as good as any other */
    char here;
    char *far_below = (char *)((unsigned long)&here - (4UL << 20));
    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "s", 1) == 1);
    CHECK(read(p[0], far_below, 1) == 1 && *far_below == 's');
    close(p[0]);
    close(p[1]);

    /* ...but not beyond the limit: that is simply a bad address */
    unsigned long beyond = (unsigned long)&here - (16UL << 20);
    CHECK(read(0, (void *)beyond, 1) == -EFAULT);
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
    CHECK(waitpid(pid, shared_status, 0) == pid);
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
        waitpid(second, &status, 0);
        exit(status == 2 && depth_value == 1 ? 3 : 1);
    }
    int status = -1;
    CHECK(waitpid(first, &status, 0) == first);
    CHECK(status == 3);
    CHECK(depth_value == 0);

    CHECK(munmap(big, PAGES * 4096) == 0);
    CHECK(munmap(shared_status, 4096) == 0);
}

int main(void) {
    check_basics();
    check_direction_flag();
    check_gs_is_ours();
    check_bad_pointers();
    check_memory();
    check_fork();
    check_copy_on_write();
    check_descriptors_and_wait();
    check_signal_masks();
    check_jobs();
    check_stack_growth();
    check_lazy_memory();
    check_files();
    check_directories();
    check_rename();
    check_environment();
    check_signals();
    check_interrupted_calls();
    check_network();
    check_tcp();
    check_netconfig_and_dns();
    check_dhcp_lease_times();
    check_shared_memory();
    check_users();
    check_password_file();
    check_time_of_day();
    check_poll();
    check_links();
    check_line_editing();
    check_wildcards();
    check_stat();
    check_truncate();
    check_text_tools();
    check_running_program_file();
    check_shell_words();
    check_shared_mappings();
    check_screen_mapping();
    check_sessions_and_terminal();
    check_shell_control();
    check_shell_options();
    check_shell_arguments();
    check_shell_redirects();
    check_shell_variables();
    check_shell_case_functions();
    check_shell_compound_redirects();
    check_shell_arith_read();
    check_shell_heredocs();
    check_line_tools();
    check_hosts_file();
    check_more_tools();
    check_tool_options();
    check_directory_tools();
    check_proc();
    check_proc_maps_fd();
    check_proc_interrupts();
    check_groups_and_setuid();
    check_supplementary_groups();
    check_sticky_and_setgid();
    check_pipes();

    if (failures == 0) {
        printf("syscheck: all checks passed\n");
        return 0;
    }
    printf("syscheck: %d check(s) failed\n", failures);
    return 1;
}
