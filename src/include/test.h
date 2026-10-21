#ifndef TEST_H
#define TEST_H

#include <stdbool.h>
#include "kprintf.h"

// Expectations checked, passed and failed so far (every EXPECT_ counts)
extern int tests_run;
extern int tests_passed;
extern int tests_failed;

#define TEST(name) \
    static void test_##name(void)

#define EXPECT_EQ(actual, expected) \
    do { \
        /* Evaluate each argument once: they may have side effects */ \
        __typeof__(actual) actual_ = (actual); \
        __typeof__(expected) expected_ = (expected); \
        if ((long long)actual_ != (long long)expected_) { \
            kprintf("    [FAIL] %s:%d: Expected %d, got %d\n", __FILE__, __LINE__, (int)expected_, (int)actual_); \
            tests_failed++; \
        } else { \
            tests_passed++; \
        } \
        tests_run++; \
    } while (0)

#define EXPECT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            kprintf("    [FAIL] %s:%d: Condition expected true\n", __FILE__, __LINE__); \
            tests_failed++; \
        } else { \
            tests_passed++; \
        } \
        tests_run++; \
    } while (0)

// Called before each test. A TCP connection finishes closing in the
// background, up to a few seconds after the program that used it has gone,
// and frees its buffers then. A test that counts memory must not start
// while that is still to come.
void test_settle(void);

#define RUN_TEST(name) \
    do { \
        test_settle(); \
        kprintf("  Running %s...\n", #name); \
        test_##name(); \
    } while (0)

// Whether the file at `path` holds exactly `text` (up to 127 bytes)
bool test_file_is(const char *path, const char *text);

// Remove `name` from the directory `dir` (vfs_unlink): 0 or an error
int test_remove(const char *dir, const char *name);

// Run every suite below, in order, and print the totals. Returns true if
// every expectation passed.
bool run_all_tests(void);

// The suites: one per file tests/test_<name>.c, each running its tests
void test_suite_pmm(void);       /* Physical frames: one and many, sized from RAM, exhaustion and recovery */
void test_suite_vmm(void);       /* Page tables: mapping and unmapping a page */
void test_suite_heap(void);      /* kmalloc and kfree: blocks, memory given back, failure refused cleanly */
void test_suite_sched(void);     /* Tasks: cooperative switching, reaping zombies, the heap under preemption */
void test_suite_syscall(void);   /* The system call path, and /bin/syscheck from ring 3 */
void test_suite_elf(void);       /* The ELF loader: hostile files, relocations, load addresses */
void test_suite_vfs(void);       /* The file tree, paths and names, open files, the initrd parser */
void test_suite_shell(void);     /* The kernel shell: every listed command runs */
void test_suite_slab(void);      /* The slab allocator: distinct objects, alignment, pages given back */
void test_suite_mmap(void);      /* Address spaces: isolation, user range checks, mmap, frame sharing, copy-on-write */
void test_suite_apic(void);      /* Local APIC, ACPI power-off data, the clean mark of the disk */
void test_suite_smp(void);       /* Several CPUs: per-CPU data, cross-CPU calls, TLB shootdown, locks */
void test_suite_virtio(void);    /* The virtio transport: capability parsing, device mappings */
void test_suite_mouse(void);     /* The mouse and the screen as devices */
void test_suite_swap(void);      /* Swapping pages out and back in */
void test_suite_storage(void);   /* The disk driver and the sector cache */
void test_suite_fat32(void);     /* The FAT32 volume: names, writes, damaged images */
void test_suite_process(void);   /* User processes: start, fault, kill, wait, leave nothing behind */
void test_suite_ext2(void);      /* The ext2 system disk: owners, writes, the check after a crash */
void test_suite_net(void);       /* The network stack: ARP, IPv4, ICMP, UDP, malformed frames */
void test_suite_tcp(void);       /* TCP: connect and close, resets, early data, large transfers, loss, dead peers, bad segments */
void test_suite_rtc(void);       /* The CMOS clock, the time of day, dates and Unix time */
void test_suite_pipe(void);      /* Pipes */
void test_suite_ansi(void);      /* The ANSI escape parser and the console */
void test_suite_priority(void);  /* Priority scheduling */
void test_suite_sem(void);       /* Semaphores and blocking */
void test_suite_procfs(void);    /* /proc */
void test_suite_tty(void);       /* The terminal's line discipline and raw mode */
void test_suite_fpu(void);       /* FPU and SSE state per task */
void test_suite_auth(void);      /* Users, groups and permission checks */
void test_suite_pcache(void);    /* The page cache: one frame per file page, coherence, reclaim, sharing */
void test_suite_klog(void);      /* The kernel message log */
void test_suite_hrtimer(void);   /* Sleeping and the clocks: precision, order, no TSC, idle CPUs left alone */
void test_suite_mount(void);     /* The mount table */
void test_suite_shm(void);       /* Shared memory segments */
void test_suite_crypto(void);    /* SHA-256 against published test vectors */
void test_suite_dev(void);       /* /dev, and the random generator (ChaCha20 against RFC 8439) */
void test_suite_bench(void);     /* Benchmarks: each must measure something */
void test_suite_lib(void);       /* String functions and ksnprintf */
void test_suite_poll(void);      /* poll: readiness of pipes, the terminal, sockets, the mouse; the wait */

// Let other tasks run until `condition` holds, for at most `max_ticks` timer
// ticks. "Yield N times" is not a wait: with more than one CPU the other
// tasks run elsewhere, and N yields here can be over before they have begun.
#define TEST_WAIT(condition, max_ticks) do { \
    extern uint64_t timer_ticks(void); \
    extern void sched_yield(void); \
    uint64_t test_wait_deadline = timer_ticks() + (max_ticks); \
    while (!(condition) && timer_ticks() < test_wait_deadline) sched_yield(); \
} while (0)

// For "nothing leaked" checks: frames that are free, plus those the kernel
// heap holds mapped. The heap grows when it must and never shrinks, so a
// workload can leave fewer free frames behind without having leaked any:
// they have become heap, available to the next allocation. When that first
// happens depends on what ran before (and, with several CPUs, in what
// order), so comparing pmm_get_free_frames() alone fails now and then.
// Likewise the page cache keeps pages of files that were mapped and are
// not any more (pcache.h): mapped nowhere, they are as good as free.
static inline size_t test_free_frames(void) {
    extern size_t pmm_get_free_frames(void);
    extern size_t kheap_get_mapped_bytes(void);
    extern size_t pcache_unmapped_pages(void);
    return pmm_get_free_frames() + kheap_get_mapped_bytes() / 4096 + pcache_unmapped_pages();
}

#endif // TEST_H
