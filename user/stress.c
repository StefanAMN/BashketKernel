/* stress: push the kernel to its limits and check that it says "no" cleanly.
 *
 *   stress mem     map memory until the kernel refuses, then give it back
 *   stress brk     the same through brk
 *   stress fork    fork until the kernel refuses, then collect every child
 *   stress cow     fork with a large dirty region when memory is nearly full
 *   stress cowsys  as cow, but the write is made by the kernel (a read() into
 *                  the shared region) instead of by the program
 *   stress file    grow a file until the kernel refuses, then delete it
 *
 * Each mode exits 0 if the kernel failed the request with an error and
 * everything still works afterwards. Run with little RAM (QEMU -m 64M).
 */
#include "ulib.h"

#define CHUNK (1UL << 20) /* 1 MiB */

static int still_alive(void) {
    /* After running out of memory, the basics must still work */
    long pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) exit(33);
    int status = 0;
    return waitpid(pid, &status) == pid && status == 33;
}

static int stress_mem(void) {
    static void *chunks[4096];
    int count = 0;

    while (count < 4096) {
        unsigned char *p = mmap_anon(CHUNK);
        if (!p) break;
        p[0] = 1;
        p[CHUNK - 1] = 2;
        chunks[count++] = p;
    }
    printf("stress mem: kernel refused after %d MiB\n", count);
    if (count == 0 || count == 4096) return 1;

    for (int i = 0; i < count; i++) {
        if (munmap(chunks[i], CHUNK) != 0) return 1;
    }

    /* Everything came back: the same amount can be mapped again */
    void *again = mmap_anon((size_t)count * CHUNK / 2);
    if (!again) return 1;
    munmap(again, (size_t)count * CHUNK / 2);
    return still_alive() ? 0 : 1;
}

static int stress_brk(void) {
    char *start = sbrk(0);
    long grown = 0;
    while (grown < 4096 && sbrk((long)CHUNK) != (void *)-1) grown++;
    printf("stress brk: kernel refused after %ld MiB\n", grown);
    if (grown == 0 || grown == 4096) return 1;

    start[0] = 1;
    start[grown * (long)CHUNK - 1] = 2;
    if (sbrk(-grown * (long)CHUNK) == (void *)-1) return 1;
    return (sbrk(0) == start && still_alive()) ? 0 : 1;
}

static int stress_fork(void) {
    static long pids[4096];
    int count = 0;

    while (count < 4096) {
        long pid = fork();
        if (pid < 0) break;
        if (pid == 0) {
            for (;;) sleep_ms(100000); /* Stay alive, holding our resources */
        }
        pids[count++] = pid;
    }
    printf("stress fork: kernel refused after %d processes\n", count);
    if (count == 0 || count == 4096) return 1;

    for (int i = 0; i < count; i++) kill(pids[i], 9);
    for (int i = 0; i < count; i++) {
        int status = 0;
        if (waitpid(pids[i], &status) != pids[i] || status != 128 + 9) return 1;
    }
    return still_alive() ? 0 : 1;
}

/* Copy-on-write promises memory it may not have: after fork, parent and child
 * share pages, and writing to them needs new frames. If none are left, the
 * writer cannot be given an error code (it is in the middle of an ordinary
 * store instruction), so the kernel has to kill it, and only it. */
static int stress_cow_common(int via_syscall);
static int stress_cow(void)    { return stress_cow_common(0); }
static int stress_cowsys(void) { return stress_cow_common(1); }

static int stress_cow_common(int via_syscall) {
    /* Take most of the memory and dirty it */
    static unsigned char *chunks[4096];
    int count = 0;
    while (count < 4096) {
        unsigned char *p = mmap_anon(CHUNK);
        if (!p) break;
        for (unsigned long i = 0; i < CHUNK; i += 4096) p[i] = 1;
        chunks[count++] = p;
    }
    /* Keep about 60%: the child cannot possibly copy all of it */
    int keep = count * 6 / 10;
    for (int i = keep; i < count; i++) munmap(chunks[i], CHUNK);
    printf("stress cow: holding %d MiB of %d\n", keep, count);
    if (keep == 0) return 1;

    long pid = fork();
    if (pid < 0) {
        printf("stress cow: fork refused (acceptable)\n");
        return still_alive() ? 0 : 1;
    }
    if (pid == 0) {
        long zero = via_syscall ? open("/dev/zero", O_RDONLY) : -1;
        for (int i = 0; i < keep; i++) {
            if (via_syscall) {
                /* The kernel fills the buffer, so the kernel takes the faults */
                if (read((int)zero, chunks[i], CHUNK) != (long)CHUNK) exit(3);
            } else {
                for (unsigned long j = 0; j < CHUNK; j += 4096) chunks[i][j] = 2;
            }
        }
        exit(0); /* Only reached if there was memory for a full copy after all */
    }

    int status = -1;
    if (waitpid(pid, &status) != pid) return 1;
    printf("stress cow: child ended with status %d\n", status);

    /* The parent's data is intact whatever happened to the child */
    for (int i = 0; i < keep; i++) {
        if (chunks[i][0] != 1 || chunks[i][CHUNK - 4096] != 1) return 1;
    }
    for (int i = 0; i < keep; i++) munmap(chunks[i], CHUNK);
    return still_alive() ? 0 : 1;
}

static int stress_file(void) {
    long fd = open("/tmp/stress.dat", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;

    static char block[65536];
    memset(block, 'x', sizeof(block));
    long written = 0, n;
    while (written < (1L << 32) && (n = write((int)fd, block, sizeof(block))) > 0) written += n;
    printf("stress file: kernel refused after %ld KiB\n", written / 1024);
    close((int)fd);

    if (written == 0 || written >= (1L << 32)) return 1;
    if (unlink("/tmp/stress.dat") != 0) return 1;
    return still_alive() ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    int result;

    if (strcmp(mode, "mem") == 0) result = stress_mem();
    else if (strcmp(mode, "brk") == 0) result = stress_brk();
    else if (strcmp(mode, "fork") == 0) result = stress_fork();
    else if (strcmp(mode, "cow") == 0) result = stress_cow();
    else if (strcmp(mode, "cowsys") == 0) result = stress_cowsys();
    else if (strcmp(mode, "file") == 0) result = stress_file();
    else {
        printf("usage: stress mem|brk|fork|cow|cowsys|file\n");
        return 2;
    }

    printf("stress %s: %s\n", mode, result == 0 ? "ok" : "FAILED");
    return result;
}
