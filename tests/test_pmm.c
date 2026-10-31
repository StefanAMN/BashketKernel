#include "test.h"
#include "pmm.h"

// One frame out and back: the free count follows
TEST(pmm_allocation) {
    size_t free_before = pmm_get_free_frames();
    void *frame = pmm_alloc_frame();
    EXPECT_TRUE(frame != NULL);
    EXPECT_EQ(pmm_get_free_frames(), free_before - 1);

    pmm_free_frame(frame);
    EXPECT_EQ(pmm_get_free_frames(), free_before);
}

// A run of four frames out and back: the free count follows
TEST(pmm_multi_allocation) {
    size_t free_before = pmm_get_free_frames();
    void *frames = pmm_alloc_frames(4);
    EXPECT_TRUE(frames != NULL);
    EXPECT_EQ(pmm_get_free_frames(), free_before - 4);

    pmm_free_frames(frames, 4);
    EXPECT_EQ(pmm_get_free_frames(), free_before);
}

// The frame count describes RAM, not the top of the physical address space
TEST(pmm_sized_from_ram) {
    // Total frames must describe RAM, not the top of the physical address
    // space: nearly all of it should have been free at boot.
    size_t total = pmm_get_total_frames();
    EXPECT_TRUE(total > 0);
    EXPECT_TRUE(pmm_get_free_frames() + pmm_get_used_frames() == total);
    EXPECT_TRUE(pmm_get_free_frames() > total / 2);
}

// Every frame can be taken, the next request fails cleanly, and all of them can
// be given back
TEST(pmm_exhaustion_and_recovery) {
    size_t free_before = pmm_get_free_frames();
    uint64_t hhdm = pmm_get_hhdm_offset();

    // Allocate every frame, chaining them through their own first 8 bytes
    void *head = NULL;
    size_t taken = 0;
    while (taken < free_before) {
        void *frame = pmm_alloc_frame();
        if (!frame) break;
        *(void **)((uint64_t)frame + hhdm) = head;
        head = frame;
        taken++;
    }

    EXPECT_TRUE(taken == free_before);
    EXPECT_TRUE(pmm_get_free_frames() == 0);
    EXPECT_TRUE(pmm_alloc_frame() == NULL); // Prints one out-of-memory error

    while (head) {
        void *next = *(void **)((uint64_t)head + hhdm);
        pmm_free_frame(head);
        head = next;
    }

    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

void test_suite_pmm(void) {
    kprintf("Suite: PMM (Physical Memory Manager)\n");
    RUN_TEST(pmm_allocation);
    RUN_TEST(pmm_multi_allocation);
    RUN_TEST(pmm_sized_from_ram);
    RUN_TEST(pmm_exhaustion_and_recovery);
}
