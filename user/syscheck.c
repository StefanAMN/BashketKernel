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
        for (;;) yield();
    }
    sleep_ms(20);
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(kill(pid, SIGINT) == 0);
    sleep_ms(20);
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
    sleep_ms(30);
    CHECK(kill(pid, SIGUSR2) == 0);
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
    sleep_ms(50);                                /* Let it get as far as going to sleep */
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
    long pid = fork();
    if (pid == 0) {
        uint32_t mine = 0;
        sigprocmask(SIG_BLOCK, NULL, &mine);
        if (mine != sigbit(SIGTERM)) exit(1);
        sleep_ms(200);                            /* The signal arrives during this, and must wait */
        sigprocmask(SIG_UNBLOCK, &set, NULL);     /* Here it takes effect */
        exit(2);
    }
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    sleep_ms(50);
    CHECK(kill(pid, SIGTERM) == 0);
    int status = -1;
    unsigned long start = clock_ns();
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGTERM);
    CHECK((clock_ns() - start) / 1000000UL >= 100);   /* Not at once: only when it unblocked */

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
        mask_calls = 0;
        action.handler = count_mask_calls;
        action.mask = 0;
        action.flags = restart ? SA_RESTART : 0;
        CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
        long parent = getpid();
        pid = fork();
        if (pid == 0) {
            sleep_ms(80);
            kill((int)parent, SIGUSR1);
            sleep_ms(80);
            write(p[1], "r", 1);
            exit(0);
        }
        long n = read(p[0], &byte, 1);
        CHECK(mask_calls == 1);
        if (restart) CHECK(n == 1 && byte == 'r');
        else CHECK(n == -EINTR);
        while (waitpid(pid, &status, 0) == -EINTR) { }
        close(p[0]);
        close(p[1]);
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
    sleep_ms(50);
    CHECK(*counter > frozen);

    /* A stop that has been asked for and cancelled before it took effect,
     * or taken back at once, leaves it running */
    CHECK(kill(pid, SIGSTOP) == 0 && kill(pid, SIGCONT) == 0);
    sleep_ms(50);
    frozen = *counter;
    sleep_ms(50);
    CHECK(*counter > frozen);

    /* A stopped process can still be killed */
    CHECK(kill(pid, SIGSTOP) == 0);
    CHECK(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGKILL);
    munmap((void *)counter, 4096);

    /* A process stopped while asleep goes back to its sleep when continued:
     * the sleep is neither cut short nor reported as interrupted */
    pid = fork();
    if (pid == 0) {
        unsigned long start = clock_ns();
        long r = syscall1(SYS_SLEEP, 300);
        unsigned long ms = (clock_ns() - start) / 1000000UL;
        exit(r == 0 && ms >= 290 ? 0 : 1);
    }
    sleep_ms(50);
    CHECK(kill(pid, SIGSTOP) == 0);
    CHECK(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
    sleep_ms(50);
    CHECK(kill(pid, SIGCONT) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);

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

/* Mapped memory costs nothing until touched (MemFree and Promised in
 * /proc/meminfo say so), and file mappings read the file */
static void check_lazy_memory(void) {
    const size_t big = 64UL << 20;                         /* 64 MiB */
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
    CHECK(syscall6(SYS_MMAP, 0, 4096, PROT_READ, MAP_SHARED, fd, 0) == -EINVAL);          /* Shared file mappings: not offered */
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

/* A signal ends a sleeping read, sleep or waitpid with -EINTR, and an
 * ignored one does not */
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
    static char big[1500];
    CHECK(sendto((int)client, big, 1472, OUR_IP, 9000) == 1472);   /* The largest datagram */
    CHECK(sendto((int)client, big, 1473, OUR_IP, 9000) == -EMSGSIZE);
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
    sleep_ms(50);
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
    static char mounts[512];
    fd = open("/proc/mounts", O_RDONLY);
    long n = fd >= 0 ? read((int)fd, mounts, sizeof(mounts) - 1) : 0;
    if (fd >= 0) close((int)fd);
    mounts[n > 0 ? n : 0] = '\0';
    int on_disk = 0;
    for (long i = 0; i + 7 < n; i++) if (memcmp(mounts + i, " / ext2", 7) == 0) on_disk = 1;
    if (!on_disk) goto clean_up;

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
        dup2((int)open("/dev/null", O_WRONLY), 2);
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
 * takes to see end-of-file: at once if the child's copy of the write end
 * was closed by the exec, a second if it was not. */
static unsigned long exec_holding_pipe(int flags) {
    int p[2];
    if (pipe2(p, flags) != 0) return 0;
    long pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/sleep", "1", NULL };
        execve("/bin/sleep", args, NULL);
        exit(99);
    }
    close(p[1]);
    unsigned long ms = ms_until_eof(p[0]);
    close(p[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    return status == 0 ? ms : 0;
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
    unsigned long kept = exec_holding_pipe(0);
    unsigned long closed = exec_holding_pipe(O_CLOEXEC);
    CHECK(kept >= 800);                                   /* Held open by the new program for its whole second */
    CHECK(closed < 500);                                  /* Gone as soon as it started */

    /* waitpid that does not wait */
    while (waitpid(-1, &status, WNOHANG) > 0) { }
    CHECK(waitpid(-1, &status, WNOHANG) == -ECHILD);      /* Nobody to wait for */
    long pid = fork();
    if (pid == 0) {
        sleep_ms(300);
        exit(7);
    }
    status = -1;
    CHECK(waitpid(pid, &status, WNOHANG) == 0 && status == -1); /* Alive: 0, and status untouched */
    CHECK(waitpid(-1, &status, WNOHANG) == 0);
    CHECK(waitpid(pid, &status, 0x40) == -EINVAL);
    int polls = 0;
    long got = 0;
    while ((got = waitpid(pid, &status, WNOHANG)) == 0 && polls < 100) {
        sleep_ms(20);
        polls++;
    }
    CHECK(got == pid && status == 7 && polls >= 5);
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
    check_shared_memory();
    check_users();
    check_password_file();
    check_time_of_day();
    check_poll();
    check_links();
    check_line_editing();
    check_wildcards();
    check_stat();
    check_text_tools();
    check_shell_words();
    check_groups_and_setuid();
    check_pipes();

    if (failures == 0) {
        printf("syscheck: all checks passed\n");
        return 0;
    }
    printf("syscheck: %d check(s) failed\n", failures);
    return 1;
}
