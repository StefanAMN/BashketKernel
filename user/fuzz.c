/* fuzz: throw random system calls at the kernel.
 *
 *   fuzz [seed] [rounds]
 *
 * Each round forks a child that makes a few hundred system calls with
 * arguments picked to cause trouble: kernel addresses, pointers one byte from
 * the end of a mapping, huge lengths, closed descriptors, odd flags. The child
 * may well get itself killed; that is fine. What must never happen is the
 * kernel crashing, hanging, or losing memory. The parent checks that every
 * round ends and prints a summary; run a normal program afterwards to see
 * that the system is still healthy.
 *
 * The same seed gives the same sequence, so a crash can be reproduced.
 */
#include "ulib.h"

#define CALLS_PER_ROUND 300
#define WATCHDOG_MS     1500

static unsigned long rng_state;

static unsigned long rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static unsigned long pick(const unsigned long *values, unsigned long count) {
    return values[rnd() % count];
}

/* Memory the child really owns, so some calls get far enough to do work */
static char scratch[8192];
static unsigned char *mapped;      /* One page from mmap */
static const char *const safe_paths[] = {
    "/tmp/fuzz-a", "/tmp/fuzz-b", "/tmp/fuzz-dir", "/tmp/fuzz-dir/x", "/tmp", "/",
    "/dev/null", "/dev/zero", "/dev/random", "/dev/vda", "/proc/tasks", "/proc/meminfo",
    "/proc", "/dev", "", "/tmp/", "//", "/tmp/fuzz-a/not-a-dir", "no-slash",
    ".", "..", "../..", "./fuzz-b", "/tmp/fuzz-dir/../fuzz-a", "/tmp/./.",
};
static const char *const programs[] = { "/bin/hello", "/bin/echo", "/tmp/fuzz-a", "/tmp", "/dev/zero" };

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static unsigned long random_pointer(void) {
    const unsigned long choices[] = {
        0,                                          /* NULL */
        1,
        0x1000,
        (unsigned long)scratch,                     /* Valid, writable */
        (unsigned long)scratch + sizeof(scratch) - 1,
        (unsigned long)mapped,
        (unsigned long)mapped + 4095,               /* Last byte of a mapping */
        (unsigned long)mapped + 4096,               /* First byte past it */
        (unsigned long)random_pointer,              /* Our code: read-only */
        (unsigned long)&choices,                    /* The stack */
        0x00007FFFFFFFEFFFUL,                       /* Top of the user stack */
        0x00007FFFFFFFF000UL,                       /* Just past it */
        0x00007FFFFFFFFFFFUL,                       /* Last user address */
        0x0000800000000000UL,                       /* First non-canonical address */
        0xFFFF800000000000UL,                       /* The kernel's direct map */
        0xFFFF900000000000UL,                       /* The kernel heap */
        0xFFFFFFFF80000000UL,                       /* The kernel image */
        0xFFFFFFFFFFFFFFFFUL,
        0xFFFFFFFFFFFFF000UL,
    };
    if (rnd() % 8 == 0) return rnd();               /* Anything at all */
    return pick(choices, COUNT(choices));
}

static unsigned long random_length(void) {
    const unsigned long choices[] = {
        0, 1, 2, 7, 8, 64, 4095, 4096, 4097, 8192, 65536, 1UL << 20, 1UL << 31, 1UL << 32,
        1UL << 47, 0x7FFFFFFFFFFFFFFFUL, 0x8000000000000000UL, 0xFFFFFFFFFFFFFFFFUL, 0xFFFFFFFFFFFFF000UL,
    };
    return pick(choices, COUNT(choices));
}

static unsigned long random_fd(void) {
    const unsigned long choices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 15, 16, 17, 255, 0x7FFFFFFF, 0x80000000UL,
                                      0xFFFFFFFFUL, 0xFFFFFFFFFFFFFFFFUL };
    return pick(choices, COUNT(choices));
}

static unsigned long random_flags(void) {
    const unsigned long choices[] = { O_RDONLY, O_WRONLY, O_RDWR, 3, O_WRONLY | O_CREAT, O_RDWR | O_CREAT | O_TRUNC,
                                      O_WRONLY | O_APPEND, O_RDONLY | O_TRUNC, 0xFFFFFFFFUL, 0x80000000UL };
    return (rnd() % 6 == 0) ? rnd() : pick(choices, COUNT(choices));
}

static unsigned long random_any(void) {
    switch (rnd() % 5) {
        case 0:  return random_pointer();
        case 1:  return random_length();
        case 2:  return random_fd();
        case 3:  return rnd() % 16;
        default: return rnd();
    }
}

static void one_call(void) {
    static const long numbers[] = {
        SYS_READ, SYS_WRITE, SYS_OPEN, SYS_CLOSE, SYS_YIELD, SYS_GETPID, SYS_LSEEK, SYS_MMAP,
        SYS_MUNMAP, SYS_BRK, SYS_PIPE, SYS_DUP2, SYS_SLEEP, SYS_FORK, SYS_EXECVE, SYS_WAITPID,
        SYS_KILL, SYS_UNAME, SYS_READDIR, SYS_GETCWD, SYS_MKDIR, SYS_UNLINK, SYS_GETPPID, SYS_TIME,
        SYS_SIGNAL, SYS_SIGACTION, SYS_SIGPROCMASK, SYS_SIGRETURN, SYS_CHDIR, SYS_SETPRIORITY, SYS_TTY_RAW, SYS_TTY_SIZE, SYS_SOCKET, SYS_NETCONFIG, SYS_DUP, SYS_PIPE2, SYS_FCNTL, SYS_BIND, SYS_CONNECT, SYS_ACCEPT, SYS_LISTEN, SYS_SHUTDOWN, SYS_SENDTO, SYS_RECVFROM, SYS_PING, SYS_SHMGET, SYS_SHMAT, SYS_SHMRM, SYS_GETUID, SYS_GETEUID, SYS_GETGID, SYS_SETGID, SYS_UMASK, SYS_SETUID, SYS_CHMOD, SYS_CHOWN,
    };

    long num = (rnd() % 10 == 0) ? (long)(rnd() % 512) : numbers[rnd() % COUNT(numbers)];
    unsigned long a = random_any(), b = random_any(), c = random_any(), d = random_any(), e = random_any();

    /* Steer the arguments so calls often get past the first check. Half the
     * time, leave them completely random. */
    int shaped = (rnd() % 2 == 0);
    switch (num) {
        case SYS_EXIT:
            return; /* Ending the round early teaches nothing */

        case SYS_READ:
            /* Never the console: nobody is typing, the child would just sleep */
            a = 3 + rnd() % 14;
            if (shaped) { b = random_pointer(); c = random_length(); }
            break;
        case SYS_WRITE:
            /* Not to the screen either: keep the log readable */
            a = 3 + rnd() % 14;
            if (shaped) { b = random_pointer(); c = random_length(); }
            break;

        case SYS_OPEN:
            a = shaped ? (unsigned long)safe_paths[rnd() % COUNT(safe_paths)] : random_pointer();
            b = random_flags();
            break;
        case SYS_CHDIR:
            a = shaped ? (unsigned long)safe_paths[rnd() % COUNT(safe_paths)] : random_pointer();
            break;
        case SYS_MKDIR:
        case SYS_UNLINK:
            /* Only ever names under /tmp, or the rest of the system would not survive the test */
            a = shaped ? (unsigned long)safe_paths[rnd() % 4] : random_pointer();
            break;
        case SYS_EXECVE:
            a = shaped ? (unsigned long)programs[rnd() % COUNT(programs)] : random_pointer();
            b = (rnd() % 2) ? 0 : random_pointer();
            if (rnd() % 4 != 0) return; /* Most rounds should keep fuzzing */
            break;

        case SYS_CLOSE:
        case SYS_LSEEK:
        case SYS_READDIR:
            if (shaped) a = rnd() % 16;
            if (num == SYS_READDIR && shaped) b = random_pointer();
            break;
        case SYS_DUP2:
            if (shaped) { a = rnd() % 16; b = rnd() % 18; }
            break;
        case SYS_PIPE:
        case SYS_UNAME:
            a = random_pointer();
            break;
        case SYS_GETCWD:
            a = random_pointer();
            b = random_length();
            break;

        case SYS_MMAP:
            if (shaped) { a = 0; b = random_length(); c = rnd() % 8; d = MAP_PRIVATE | MAP_ANONYMOUS; }
            if (b > (64UL << 20) && b < (1UL << 40)) b = 4096 * (1 + rnd() % 64); /* Do not eat all RAM slowly */
            break;
        case SYS_MUNMAP:
            if (shaped) { a = (unsigned long)mapped + 4096 * (rnd() % 4); b = random_length(); }
            break;
        case SYS_BRK:
            if (shaped) a = (unsigned long)sbrk(0) + (long)(rnd() % (1UL << 22)) - (1L << 21);
            break;

        case SYS_SLEEP:
            a = rnd() % 3; /* Milliseconds */
            break;
        case SYS_SETUID:
            /* Giving up root would make most later calls fail on permissions
             * instead of reaching the code they are meant to exercise */
            if (rnd() % 30 != 0) return;
            break;
        case SYS_CHMOD:
        case SYS_CHOWN: {
            /* We run as root, so these really change things. Only names
             * under /tmp, or pointers that cannot be a path at all: a random
             * valid pointer may happen to point at the text "/" in our own
             * memory, and then the root directory changes hands. */
            static const unsigned long not_paths[] = { 0, 1, 0x0000800000000000UL, 0xFFFF800000000000UL,
                                                       0xFFFFFFFF80000000UL, 0xFFFFFFFFFFFFFFFFUL };
            a = shaped ? (unsigned long)safe_paths[rnd() % 4] : pick(not_paths, COUNT(not_paths));
            break;
        }
        case SYS_SHMGET:
            if (shaped) { a = 0xF000 + rnd() % 4; b = 4096 * (1 + rnd() % 8); }
            break;
        case SYS_SHMAT:
        case SYS_SHMRM:
            if (shaped) a = rnd() % 18;
            break;
        case SYS_RECVFROM:
            e = rnd() % 20 + 1;   /* A timeout in ms: never wait for ever on a socket nobody writes to */
            if (shaped) { a = 3 + rnd() % 14; b = random_pointer(); c = random_length(); d = (rnd() % 2) ? 0 : random_pointer(); }
            break;
        case SYS_SENDTO:
            if (shaped) { a = 3 + rnd() % 14; b = random_pointer(); c = rnd() % 1600; d = 0x0A00020F; e = 1 + rnd() % 65535; }
            break;
        case SYS_PING:
            /* Only our own address: pinging random ones would wait for ARP timeouts */
            a = 0x0A00020F; b = rnd() % 20;
            break;
        case SYS_SIGRETURN:
            /* With no signal frame on the stack this ends the process, which
             * is the right answer but also ends the round: keep it rare */
            if (rnd() % 60 != 0) return;
            break;
        case SYS_SIGNAL:
            if (shaped) { a = 1 + rnd() % 31; b = (rnd() % 3 == 0) ? rnd() % 2 : random_pointer(); c = random_pointer(); }
            break;
        case SYS_KILL:
            /* Only ourselves or nobody: the test harness must survive */
            a = (rnd() % 40 == 0) ? (unsigned long)getpid() : 100000 + rnd() % 1000;
            break;
        case SYS_WAITPID:
            if (shaped) { a = (unsigned long)-1; b = (rnd() % 2) ? 0 : random_pointer(); }
            break;
        default:
            break;
    }

    long result = syscall6(num, (long)a, (long)b, (long)c, (long)d, (long)e, (long)random_any());

    /* A forked copy must not start a fuzzing dynasty */
    if (num == SYS_FORK && result == 0) exit(0);
}

static void fuzz_child(unsigned long seed) {
    rng_state = seed;
    mapped = mmap_anon(4096);

    /* A few open descriptors of each kind for the calls to chew on */
    int fds[2];
    open("/tmp/fuzz-a", O_RDWR | O_CREAT);
    open("/dev/zero", O_RDONLY);
    open("/proc/tasks", O_RDONLY);
    open("/tmp", O_RDONLY);
    if (pipe(fds) == 0) write(fds[1], "seed data", 9);
    long sock = socket(SOCK_DGRAM);
    if (sock >= 0) bind((int)sock, 20000 + (unsigned)(seed % 1000));

    for (int i = 0; i < CALLS_PER_ROUND; i++) one_call();
    exit(0);
}

int main(int argc, char **argv) {
    unsigned long seed = argc > 1 ? (unsigned long)atoi(argv[1]) : 1;
    long rounds = argc > 2 ? atoi(argv[2]) : 50;
    if (seed == 0) seed = 1;

    int ended_normally = 0, killed = 0, timed_out = 0;

    /* The children run as root and may hit on a valid way to change the
     * machine's address: put the network back after every round */
    struct net_config network;
    bool have_network = netconfig(&network, NULL) == 0;

    for (long round = 0; round < rounds; round++) {
        unsigned long round_seed = seed * 0x9E3779B97F4A7C15UL + (unsigned long)round * 0xBF58476D1CE4E5B9UL + 1;

        long child = fork();
        if (child == 0) fuzz_child(round_seed);
        if (child < 0) {
            printf("fuzz: FAILED: fork: %s (round %ld)\n", strerror(child), round);
            return 1;
        }

        /* A watchdog, because a child may block forever (waiting on a pipe
         * it holds both ends of, say) */
        long watchdog = fork();
        if (watchdog == 0) {
            sleep_ms(WATCHDOG_MS);
            kill(child, 9);
            exit(1);
        }

        int status = -1, watchdog_status = -1;
        if (waitpid(child, &status, 0) != child) {
            printf("fuzz: FAILED: lost track of the child (round %ld)\n", round);
            return 1;
        }
        kill(watchdog, 9);
        waitpid(watchdog, &watchdog_status, 0);

        if (watchdog_status == 1) timed_out++;
        else if (status >= 128) killed++;
        else ended_normally++;

        /* Children the fuzzed process forked are ours to collect now only if
         * they were re-parented; this kernel just detaches them, so nothing
         * to do. Clean up the files the round may have left. */
        if (have_network) netconfig(NULL, &network);
        unlink("/tmp/fuzz-dir/x");
        unlink("/tmp/fuzz-dir");
        unlink("/tmp/fuzz-a");
        unlink("/tmp/fuzz-b");
    }

    printf("fuzz: seed %lu, %ld rounds: %d ended normally, %d killed, %d stopped by watchdog\n",
           seed, rounds, ended_normally, killed, timed_out);
    printf("fuzz: done\n");
    return 0;
}
