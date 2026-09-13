#include "test.h"
#include "pmm.h"
#include "heap.h"
#include <string.h>

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
    EXPECT_TRUE(kheap_get_mapped_bytes() >= mapped_before + big - KHEAP_TRIM_THRESHOLD);
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

void test_suite_heap(void) {
    kprintf("Suite: Kernel Heap (kmalloc / kfree)\n");
    RUN_TEST(heap_allocation);
    RUN_TEST(heap_multiple_allocations);
    RUN_TEST(heap_returns_memory_and_refuses_cleanly);
}
