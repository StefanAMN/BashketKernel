/* threads: several threads on one address space, on as many CPUs as there are.
 *
 *   threads [rounds]
 *
 * Each part fails in a way only sharing can: a page given twice by two faults
 * at once loses a thread's count; a page marked copy-on-write that another CPU
 * can still write through changes under a forked child; a page unmapped that
 * another CPU can still read lets the reader run on; a file page installed over
 * one another thread has already written loses the write. Exit status 0 only if
 * all held. Mostly meant for four CPUs and more (make test-smp).
 */
#include "ulib.h"

#define WORKERS 4
#define TOUCH_PAGES 256
#define PAGE 4096

static volatile unsigned long *touched;
static volatile int failed;
static long rounds = 3;

/* Report a failed check; main returns non-zero for it */
static void fail(const char *what) {
    printf("threads: FAILED: %s\n", what);
    failed = 1;
}

/* Add one to a word on every page, all threads the same pages at once, so that
 * most first touches race with another */
static int toucher(void *unused) {
    (void)unused;
    for (long page = 0; page < TOUCH_PAGES; page++) {
        __atomic_add_fetch(&touched[page * PAGE / sizeof(unsigned long)], 1, __ATOMIC_RELAXED);
    }
    return 0;
}

/* Two faults on one page give it one frame: no thread's count is lost */
static void check_first_touch(void) {
    touched = mmap_anon(TOUCH_PAGES * PAGE);
    if (!touched) return fail("no memory");
    thread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) if (thread_create(&t[i], toucher, 0) != 0) fail("thread_create");
    for (int i = 0; i < WORKERS; i++) if (thread_join(&t[i], 0) != 0) fail("thread_join");
    for (long page = 0; page < TOUCH_PAGES; page++) {
        if (touched[page * PAGE / sizeof(unsigned long)] != WORKERS) fail("a first touch was lost");
    }
    munmap((void *)touched, TOUCH_PAGES * PAGE);
}

#define FILE_PAGES 24
#define FILE_WORKERS 4

static volatile char *file_memory;
static volatile int file_go;

/* Wait for the word, then write this thread's mark on every page */
static int file_toucher(void *arg) {
    long me = (long)arg;
    while (!file_go) { }
    for (long page = 0; page < FILE_PAGES; page++) file_memory[page * PAGE + me] = (char)(me + 1);
    return 0;
}

/* The first touches of a private file mapping, with the file not yet in the
 * page cache: the fault reads the disk with its lock released, and the others
 * fault meanwhile. A slower one must look again before it installs its page,
 * or it replaces a page holding another thread's mark. In a boot that has not
 * read /disk/pattern.bin its pages are cold, and only then does this test the
 * race (so only the first run in a boot, and no rounds). */
static void check_file_first_touch(void) {
    long fd = open("/disk/pattern.bin", O_RDONLY);
    if (fd < 0) return fail("cannot open /disk/pattern.bin");
    file_memory = mmap_file((int)fd, FILE_PAGES * PAGE, 0, PROT_READ | PROT_WRITE);
    if (!file_memory) return fail("no mapping");
    file_go = 0;
    thread_t t[FILE_WORKERS];
    for (long i = 0; i < FILE_WORKERS; i++) if (thread_create(&t[i], file_toucher, (void *)i) != 0) fail("thread_create");
    file_go = 1;
    for (int i = 0; i < FILE_WORKERS; i++) if (thread_join(&t[i], 0) != 0) fail("thread_join");
    for (long page = 0; page < FILE_PAGES; page++) {
        for (long i = 0; i < FILE_WORKERS; i++) {
            if (file_memory[page * PAGE + i] != (char)(i + 1)) return fail("a write to a page being read in was lost");
        }
        /* What the disk image builder put there (tests/test_fat32.c) */
        long at = page * PAGE + 100;
        if (file_memory[at] != (char)(at * 7 + (at >> 8) * 13)) return fail("a file page came back wrong");
    }
    munmap((void *)file_memory, FILE_PAGES * PAGE);
    close((int)fd);
}

static volatile int writers_stop;
static volatile unsigned long write_counters[2];

/* Keep writing to one word */
static int writer(void *arg) {
    volatile unsigned long *word = arg;
    while (!writers_stop) (*word)++;
    return 0;
}

/* After fork, the child's memory is a snapshot: other threads of the parent
 * still writing must not reach it through a translation they had before */
static void check_fork_snapshot(void) {
    writers_stop = 0;
    thread_t t[2];
    for (int i = 0; i < 2; i++) if (thread_create(&t[i], writer, (void *)&write_counters[i]) != 0) fail("thread_create");
    for (long round = 0; round < 20 * rounds; round++) {
        long pid = fork();
        if (pid == 0) {
            unsigned long a = write_counters[0], b = write_counters[1];
            sleep_ms(5);
            exit(a == write_counters[0] && b == write_counters[1] ? 0 : 1);
        }
        int status = -1;
        if (pid < 0 || waitpid(pid, &status, 0) != pid || status != 0) fail("a forked child's memory changed under it");
    }
    writers_stop = 1;
    for (int i = 0; i < 2; i++) thread_join(&t[i], 0);
}

/* Map, touch and give back, over and over, against other threads doing so */
static int mapper(void *unused) {
    (void)unused;
    for (long i = 0; i < 100 * rounds; i++) {
        char *p = mmap_anon(8 * PAGE);
        if (!p) return 1;
        for (int page = 0; page < 8; page++) p[page * PAGE] = (char)page;
        for (int page = 0; page < 8; page++) if (p[page * PAGE] != (char)page) return 2;
        if (munmap(p, 8 * PAGE) != 0) return 3;
    }
    return 0;
}

/* Open, read and close, so that other threads' descriptors come and go */
static int opener(void *unused) {
    (void)unused;
    char text[64];
    for (long i = 0; i < 200 * rounds; i++) {
        long fd = open("/etc/motd", O_RDONLY);
        if (fd < 0) return 1;
        if (read((int)fd, text, sizeof(text)) < 0) return 2;
        close((int)fd);
    }
    return 0;
}

/* Close descriptors another thread is reading: only what it has not got yet
 * may fail, and nothing may crash. (All lowest-free numbers: they collide.) */
static int closer(void *unused) {
    (void)unused;
    for (long i = 0; i < 300 * rounds; i++) {
        for (int fd = 3; fd < 8; fd++) {
            char c;
            read(fd, &c, 0);
            lseek(fd, 0, SEEK_SET);
            if (i % 7 == 0) close(fd);
        }
    }
    return 0;
}

/* Everything that changes the mappings and the table at the same time */
static void check_churn(void) {
    thread_t t[5];
    int (*const jobs[5])(void *) = { mapper, mapper, opener, opener, closer };
    for (int i = 0; i < 5; i++) if (thread_create(&t[i], jobs[i], 0) != 0) fail("thread_create");
    for (int i = 0; i < 5; i++) {
        int status = -1;
        thread_join(&t[i], &status);
        /* An opener may lose its descriptor to the closer: read then says so */
        if (i != 2 && i != 3 && status != 0) fail("a thread saw its memory or files go wrong");
    }
}

#define RACERS 2

static volatile long race_round;
static volatile int race_ready;
static volatile int race_op;           /* 0: close race_fd; 1: dup descriptor 0 */
static volatile long race_fd;
static volatile int race_closed;
static volatile long race_dups[RACERS];

/* At each new round, do the round's one call at the same moment as the other */
static int racer(void *arg) {
    long me = (long)arg, seen = 0;
    for (;;) {
        while (race_round == seen) yield();
        seen = race_round;
        if (seen < 0) return 0;
        __atomic_add_fetch(&race_ready, 1, __ATOMIC_ACQ_REL);
        while (race_ready < RACERS) {}
        if (race_op == 0) {
            if (close((int)race_fd) == 0) __atomic_add_fetch(&race_closed, 1, __ATOMIC_ACQ_REL);
        } else {
            race_dups[me] = dup(0);
        }
        __atomic_add_fetch(&race_ready, 1, __ATOMIC_ACQ_REL);
    }
}

/* Start one round and wait for both racers to have made their call */
static void race(int op) {
    race_op = op;
    race_ready = 0;
    race_round++;
    while (race_ready < 2 * RACERS) yield();
}

/* Of two threads closing one descriptor exactly one succeeds; two taking the
 * lowest free one at once get different ones */
static void check_descriptor_races(void) {
    thread_t t[RACERS];
    race_round = 0;
    for (long i = 0; i < RACERS; i++) if (thread_create(&t[i], racer, (void *)i) != 0) fail("thread_create");
    for (long round = 0; round < 500 * rounds; round++) {
        race_fd = dup(0);
        race_closed = 0;
        race(0);
        if (race_closed != 1) fail("one descriptor closed twice, or not at all");
        race(1);
        if (race_dups[0] < 0 || race_dups[0] == race_dups[1]) fail("two threads were given one descriptor");
        close((int)race_dups[0]);
        if (race_dups[1] != race_dups[0]) close((int)race_dups[1]);
    }
    race_round = -1;
    for (int i = 0; i < RACERS; i++) thread_join(&t[i], 0);
}

static volatile char *watched;

/* Read the same byte for ever: only a fault can end this */
static int reader(void *unused) {
    (void)unused;
    for (;;) (void)*watched;
    return 0;
}

/* A page unmapped by one thread is gone for one spinning on another CPU, which
 * has it in its TLB and no reason to enter the kernel */
static void check_unmap_reaches_other_cpus(void) {
    long pid = fork();
    if (pid == 0) {
        watched = mmap_anon(PAGE);
        if (!watched) exit(2);
        *watched = 1;
        thread_t t;
        if (thread_create(&t, reader, 0) != 0) exit(3);
        sleep_ms(50);
        munmap((void *)watched, PAGE);
        sleep_ms(3000);          /* The reader's fault ends the process long before this */
        exit(9);
    }
    int status = -1;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) return fail("waitpid");
    if (status != 128 + SIGSEGV) fail("a thread went on reading an unmapped page");
}

static volatile int own_stop;

/* Set FS to a word on this stack holding this thread's number, and keep finding
 * that number through FS while the threads move between CPUs */
static int own_data(void *arg) {
    volatile long mine = (long)arg;
    if (get_fs_base() != NULL) return 1;     /* A new thread has none */
    if (set_fs_base((void *)&mine) != 0) return 2;
    while (!own_stop) {
        long seen;
        asm volatile("mov %0, fs:[0]" : "=r"(seen));
        if (seen != (long)arg) return 3;
        yield();
    }
    return get_fs_base() == (void *)&mine ? 0 : 4;
}

/* More threads than CPUs, each with its own FS base: every switch must load
 * the one coming in, on whichever CPU it lands */
static void check_own_fs_base(void) {
    thread_t t[8];
    own_stop = 0;
    for (long i = 0; i < 8; i++) if (thread_create(&t[i], own_data, (void *)(i + 100)) != 0) fail("thread_create");
    sleep_ms(200);
    own_stop = 1;
    for (int i = 0; i < 8; i++) {
        int status = -1;
        thread_join(&t[i], &status);
        if (status != 0) fail("a thread found another's data through FS");
    }
}

int main(int argc, char **argv) {
    if (argc > 1) rounds = atoi(argv[1]);
    for (long i = 0; i < rounds; i++) check_first_touch();
    check_file_first_touch();
    check_fork_snapshot();
    check_descriptor_races();
    check_churn();
    check_unmap_reaches_other_cpus();
    check_own_fs_base();
    if (failed) return 1;
    printf("threads: all checks passed\n");
    return 0;
}
