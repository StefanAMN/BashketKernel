#include "test.h"
#include "shm.h"
#include "vmm.h"
#include "pmm.h"
#include "cow.h"
#include "mmap.h"
#include "heap.h"
#include "kerrno.h"

TEST(shm_segments_are_shared_and_freed) {
    size_t frames_before = pmm_get_free_frames();
    size_t heap_before = kheap_get_allocated_bytes();
    uint64_t hhdm = pmm_get_hhdm_offset();

    int id = shm_get(0x5EED, 3 * PAGE_SIZE);
    EXPECT_TRUE(id >= 0);
    EXPECT_EQ(shm_get(0x5EED, PAGE_SIZE), id);                 // Same key: the same segment
    EXPECT_EQ(shm_get(0x5EED, 4 * PAGE_SIZE), -EINVAL);        // ... which is not that large
    EXPECT_TRUE(shm_count() == 1);
    EXPECT_TRUE(pmm_get_free_frames() == frames_before - 3);

    // Two address spaces attach: the same frames behind both
    uint64_t *a = vmm_create_address_space(), *b = vmm_create_address_space();
    uint64_t next_a = USER_MMAP_BASE, next_b = USER_MMAP_BASE + 16 * PAGE_SIZE;
    uint64_t at_a = shm_attach(id, a, &next_a), at_b = shm_attach(id, b, &next_b);
    EXPECT_TRUE(at_a == USER_MMAP_BASE && at_b == USER_MMAP_BASE + 16 * PAGE_SIZE);
    EXPECT_TRUE(next_a == at_a + 3 * PAGE_SIZE);
    for (int page = 0; page < 3; page++) {
        uint64_t phys = vmm_get_phys(a, at_a + (uint64_t)page * PAGE_SIZE);
        EXPECT_TRUE(phys != 0 && phys == vmm_get_phys(b, at_b + (uint64_t)page * PAGE_SIZE));
        EXPECT_EQ(pmm_get_frame_ref((void *)phys), 3u);        // The table and two mappings
        EXPECT_EQ(((uint8_t *)(phys + hhdm))[100], 0);         // Starts out zeroed
    }
    uint64_t pte = vmm_get_pte(a, at_a);
    EXPECT_TRUE((pte & VMM_SHARED) && (pte & VMM_WRITABLE) && (pte & VMM_USER) && (pte & VMM_NO_EXECUTE));

    // A fork of one of them shares the segment too, writable, not copy-on-write
    uint64_t *child = vmm_create_address_space();
    EXPECT_TRUE(vmm_clone_user_space(child, a));
    uint64_t child_pte = vmm_get_pte(child, at_a);
    EXPECT_TRUE((child_pte & VMM_WRITABLE) && !(child_pte & VMM_COW));
    EXPECT_TRUE(vmm_get_phys(child, at_a) == vmm_get_phys(a, at_a));
    EXPECT_TRUE(vmm_get_pte(a, at_a) & VMM_WRITABLE);          // The parent's mapping was left alone

    // Removing the segment frees the key at once, but not memory in use
    uint64_t first_frame = vmm_get_phys(a, at_a);
    EXPECT_EQ(shm_remove(id), 0);
    EXPECT_EQ(shm_remove(id), -EINVAL);
    EXPECT_TRUE(shm_count() == 0);
    EXPECT_TRUE(shm_attach(id, b, &next_b) == 0);              // Nothing to attach to any more
    EXPECT_EQ(pmm_get_frame_ref((void *)first_frame), 3u);     // a, b and the fork still map it
    EXPECT_TRUE(vmm_get_phys(a, at_a) == first_frame);

    // The memory goes with the last mapping
    vmm_destroy_address_space(a);
    vmm_destroy_address_space(child);
    EXPECT_EQ(pmm_get_frame_ref((void *)first_frame), 1u);
    vmm_destroy_address_space(b);
    EXPECT_TRUE(pmm_get_free_frames() == frames_before);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

TEST(shm_limits) {
    size_t frames_before = pmm_get_free_frames();

    EXPECT_EQ(shm_get(1, 0), -EINVAL);
    EXPECT_EQ(shm_get(1, (size_t)(SHM_MAX_PAGES + 1) * PAGE_SIZE), -EINVAL);
    EXPECT_EQ(shm_get(1, (size_t)-1), -EINVAL);
    EXPECT_EQ(shm_remove(-1), -EINVAL);
    EXPECT_EQ(shm_remove(SHM_MAX_SEGMENTS), -EINVAL);
    uint64_t next = USER_MMAP_BASE;
    EXPECT_TRUE(shm_attach(3, vmm_get_kernel_pml4(), &next) == 0);   // No such segment

    // The table has a fixed size; a full table refuses new keys but still
    // finds existing ones
    int ids[SHM_MAX_SEGMENTS];
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        ids[i] = shm_get(1000 + (uint32_t)i, PAGE_SIZE);
        EXPECT_TRUE(ids[i] >= 0);
    }
    EXPECT_EQ(shm_get(9999, PAGE_SIZE), -ENOSPC);
    EXPECT_EQ(shm_get(1005, PAGE_SIZE), ids[5]);
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) EXPECT_EQ(shm_remove(ids[i]), 0);

    EXPECT_TRUE(pmm_get_free_frames() == frames_before);        // Refused requests kept nothing
}

void test_suite_shm(void) {
    kprintf("Suite: Shared Memory\n");
    RUN_TEST(shm_segments_are_shared_and_freed);
    RUN_TEST(shm_limits);
}
