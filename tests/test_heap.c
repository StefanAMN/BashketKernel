#include "test.h"
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

void test_suite_heap(void) {
    kprintf("Suite: Kernel Heap (kmalloc / kfree)\n");
    RUN_TEST(heap_allocation);
    RUN_TEST(heap_multiple_allocations);
}
