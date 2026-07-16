#include "test.h"
#include "swap.h"

TEST(swap_out_in_test) {
    swap_init(512);

    size_t initial_free = swap_get_free_slots();
    EXPECT_TRUE(initial_free == 512);

    uint32_t slot = swap_out_frame(0x200000);
    EXPECT_TRUE(slot != 0xFFFFFFFF);
    EXPECT_TRUE(swap_get_free_slots() == initial_free - 1);

    uint64_t phys = swap_in_frame(slot);
    EXPECT_TRUE(phys != 0);
    EXPECT_TRUE(swap_get_free_slots() == initial_free);
}

void test_suite_swap(void) {
    kprintf("Suite: Phase 27 Virtual Memory Swap Manager\n");
    RUN_TEST(swap_out_in_test);
}
