#include "test.h"
#include "mmap.h"
#include "string_util.h"

TEST(mmap_anonymous_test) {
    size_t alloc_len = 8192; // 2 pages
    char *ptr = (char *)mmap(NULL, alloc_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    EXPECT_TRUE(ptr != MAP_FAILED);
    EXPECT_TRUE(ptr != NULL);

    // Verify zeroed out
    EXPECT_EQ(ptr[0], 0);
    EXPECT_EQ(ptr[4096], 0);

    // Write and read
    ptr[0] = 'M';
    ptr[4096] = 'P';
    EXPECT_EQ(ptr[0], 'M');
    EXPECT_EQ(ptr[4096], 'P');

    int res = munmap(ptr, alloc_len);
    EXPECT_EQ(res, 0);
}

void test_suite_mmap(void) {
    kprintf("Suite: Phase 8 Virtual Memory Mapping (mmap/munmap)\n");
    RUN_TEST(mmap_anonymous_test);
}
