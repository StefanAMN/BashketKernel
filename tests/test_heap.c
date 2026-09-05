#include "test.h"
#include "pmm.h"
#include "heap.h"
#include <string.h>
#include "hrtimer.h"

TEST(heap_allocation) {
    char *str = (char *)kmalloc(64);
    EXPECT_TRUE(str != NULL);

    // Verify read/write
    for (int i = 0; i < 63; i++) {
        str[i] = 'A' + (i % 26);
    }
    str[63] = '\0';

    EXPECT_EQ(str[0], 'A');
    EXPECT_EQ(str[25], 'Z');

    kfree(str);
}

TEST(heap_multiple_allocations) {
    int *arr1 = (int *)kmalloc(100 * sizeof(int));
    int *arr2 = (int *)kmalloc(200 * sizeof(int));

    EXPECT_TRUE(arr1 != NULL);
    EXPECT_TRUE(arr2 != NULL);

    arr1[0] = 42;
    arr2[0] = 99;

    EXPECT_EQ(arr1[0], 42);
    EXPECT_EQ(arr2[0], 99);

    kfree(arr1);
    kfree(arr2);
}

TEST(heap_returns_memory_and_refuses_cleanly) {
    size_t mapped_before = kheap_get_mapped_bytes();
    size_t free_before = pmm_get_free_frames();
    const size_t big = 8 * 1024 * 1024;

    // A large allocation grows the heap ...
    uint8_t *block = kmalloc(big);
    EXPECT_TRUE(block != NULL);
    // (by the 8 MiB less whatever free space it already had, which is at
    // most the heap's initial size)
    EXPECT_TRUE(kheap_get_mapped_bytes() >= mapped_before + big - KHEAP_INITIAL_SIZE);
    block[0] = 1;
    block[big - 1] = 2;                      // Both ends are really backed by memory

    // ... and freeing it gives the frames back, apart from a small reserve
    kfree(block);
    EXPECT_TRUE(kheap_get_mapped_bytes() <= mapped_before + KHEAP_TRIM_THRESHOLD);
    // (Page tables for the range stay, so allow for those: 8 MiB needs 4-5.)
    EXPECT_TRUE(pmm_get_free_frames() + 8 + KHEAP_TRIM_THRESHOLD / 4096 >= free_before);

    // Doing it again must not consume more: nothing is lost per round
    size_t free_after_first = pmm_get_free_frames();
    for (int i = 0; i < 5; i++) kfree(kmalloc(big));
    EXPECT_TRUE(pmm_get_free_frames() == free_after_first);

    // Requests that cannot be met fail without side effects
    size_t refused_before = kheap_get_failed_allocations();
    size_t free_now = pmm_get_free_frames();
    EXPECT_TRUE(kmalloc(KHEAP_MAX_ALLOC + 1) == NULL);
    EXPECT_TRUE(kmalloc((size_t)-1) == NULL);
    EXPECT_TRUE(kmalloc((size_t)-16) == NULL);            // Would overflow when rounded up
    EXPECT_TRUE(kmalloc(0) == NULL);
    EXPECT_TRUE(pmm_get_free_frames() == free_now);
    EXPECT_TRUE(kheap_get_failed_allocations() == refused_before);   // Refused by size, not by lack of memory

    // The heap still works afterwards
    void *small = kmalloc(64);
    EXPECT_TRUE(small != NULL);
    kfree(small);
}

// Many blocks of random sizes, allocated and freed in random order: every
// block keeps its contents, and the heap's own bookkeeping stays consistent
// (kheap_check walks all of it) and comes back to where it started
TEST(heap_survives_random_use) {
    static uint8_t *held[256];
    static size_t sizes[256];
    size_t bytes_before = kheap_get_allocated_bytes();
    uint32_t seed = 12345;
    for (int round = 0; round < 4000; round++) {
        seed = seed * 1103515245u + 12345u;
        int slot = (int)((seed >> 16) % 256);
        if (held[slot]) {
            for (size_t i = 0; i < sizes[slot]; i += 61) {
                if (held[slot][i] != (uint8_t)(slot ^ i)) { EXPECT_TRUE(false); break; }
            }
            kfree(held[slot]);
            held[slot] = NULL;
        } else {
            seed = seed * 1103515245u + 12345u;
            sizes[slot] = 1 + (seed >> 16) % ((seed & 1) ? 64 : 9000);   // Mostly small, some large
            held[slot] = kmalloc(sizes[slot]);
            EXPECT_TRUE(held[slot] != NULL && ((uint64_t)held[slot] & 15) == 0);
            if (held[slot]) for (size_t i = 0; i < sizes[slot]; i++) held[slot][i] = (uint8_t)(slot ^ i);
        }
        if (round % 500 == 0) EXPECT_TRUE(kheap_check());
    }
    for (int slot = 0; slot < 256; slot++) {
        kfree(held[slot]);
        held[slot] = NULL;
    }
    EXPECT_TRUE(kheap_check());
    EXPECT_TRUE(kheap_get_allocated_bytes() == bytes_before);
}

// Freed neighbours merge on both sides, so the space can be used whole again;
// freeing twice is refused rather than corrupting the lists
TEST(heap_merges_neighbours) {
    uint8_t *a = kmalloc(4000), *b = kmalloc(4000), *c = kmalloc(4000);
    EXPECT_TRUE(a && b && c && b > a && c > b);
    kfree(a);
    kfree(c);
    kfree(b);                                   // Merges with both
    EXPECT_TRUE(kheap_check());
    uint8_t *whole = kmalloc(12000);            // Fits in the merged space, where a was
    EXPECT_TRUE(whole == a);
    kfree(whole);
    kfree(whole);                               // Reported and ignored
    EXPECT_TRUE(kheap_check());
}

// Freed memory is poisoned: a read after kfree sees the pattern, and a write
// after kfree is found by kheap_check and by the kmalloc that hands the
// block out again (here counted; anywhere else it is a panic)
TEST(heap_catches_writes_after_free) {
    uint8_t *before = kmalloc(256), *victim = kmalloc(256), *after = kmalloc(256);
    EXPECT_TRUE(before && victim && after);
    for (int i = 0; i < 256; i++) victim[i] = 0x11;
    kfree(victim);                                    // Its neighbours are in use: it stays a block of its own
    EXPECT_TRUE(victim[100] == 0x6b && victim[255] == 0x6b);
    EXPECT_TRUE(kheap_check());

    victim[100] = 0;                                  // The bug
    EXPECT_TRUE(!kheap_check());
    size_t damage = kheap_poison_damage();
    kheap_poison_damage_is_fatal(false);
    uint8_t *again = kmalloc(256);                    // The same block, first on its list
    kheap_poison_damage_is_fatal(true);
    EXPECT_TRUE(again == victim);
    EXPECT_EQ(kheap_poison_damage(), damage + 1);

    kfree(again);                                     // Poisoned afresh: nothing to report any more
    EXPECT_TRUE(kheap_check());
    uint8_t *clean = kmalloc(256);
    EXPECT_EQ(kheap_poison_damage(), damage + 1);
    kfree(clean);
    kfree(before);
    kfree(after);
    EXPECT_TRUE(kheap_check());
}

// The cost of kmalloc and kfree does not grow with the number of blocks in
// use: the previous allocator walked every block in both
TEST(heap_speed_does_not_depend_on_blocks_in_use) {
    static void *held[3000];
    // Time 2000 small kmalloc/kfree pairs with nothing extra held, then with
    // 3000 more blocks in use; best of three each, against noise
    uint64_t alone = (uint64_t)-1, crowded = (uint64_t)-1;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t start = clock_ns();
        for (int i = 0; i < 2000; i++) kfree(kmalloc(48));
        uint64_t t = clock_ns() - start;
        if (t < alone) alone = t;
    }
    for (int i = 0; i < 3000; i++) held[i] = kmalloc(48);
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t start = clock_ns();
        for (int i = 0; i < 2000; i++) kfree(kmalloc(48));
        uint64_t t = clock_ns() - start;
        if (t < crowded) crowded = t;
    }
    for (int i = 0; i < 3000; i++) kfree(held[i]);
    kprintf("    2000 kmalloc/kfree: %lu us alone, %lu us with 3000 blocks in use\n", alone / 1000, crowded / 1000);
    EXPECT_TRUE(crowded < 3 * alone + 2000000);  // (2 ms of slack for a slow host)
    EXPECT_TRUE(kheap_check());
}

void test_suite_heap(void) {
    kprintf("Suite: Kernel Heap (kmalloc / kfree)\n");
    RUN_TEST(heap_allocation);
    RUN_TEST(heap_multiple_allocations);
    RUN_TEST(heap_returns_memory_and_refuses_cleanly);
    RUN_TEST(heap_survives_random_use);
    RUN_TEST(heap_merges_neighbours);
    RUN_TEST(heap_catches_writes_after_free);
    RUN_TEST(heap_speed_does_not_depend_on_blocks_in_use);
}
