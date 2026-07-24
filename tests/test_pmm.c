#include "../src/test.h"
#include "../src/pmm.h"

TEST(pmm_allocation) {
    size_t free_before = pmm_get_free_frames();
    void *frame = pmm_alloc_frame();
    EXPECT_TRUE(frame != NULL);
    EXPECT_EQ(pmm_get_free_frames(), free_before - 1);

    pmm_free_frame(frame);
    EXPECT_EQ(pmm_get_free_frames(), free_before);
}

TEST(pmm_multi_allocation) {
    size_t free_before = pmm_get_free_frames();
    void *frames = pmm_alloc_frames(4);
    EXPECT_TRUE(frames != NULL);
    EXPECT_EQ(pmm_get_free_frames(), free_before - 4);

    pmm_free_frames(frames, 4);
    EXPECT_EQ(pmm_get_free_frames(), free_before);
}

void test_suite_pmm(void) {
    kprintf("Suite: PMM (Physical Memory Manager)\n");
    RUN_TEST(pmm_allocation);
    RUN_TEST(pmm_multi_allocation);
}
