#include "test.h"
#include "mmap.h"
#include "vmm.h"
#include "pmm.h"
#include "cow.h"

TEST(address_space_is_isolated_and_freed) {
    size_t free_before = pmm_get_free_frames();

    uint64_t *a = vmm_create_address_space();
    uint64_t *b = vmm_create_address_space();
    EXPECT_TRUE(a != NULL && b != NULL && a != b);

    // Same user address in two spaces is backed by different frames
    uint8_t *page_a = vmm_alloc_user_page(a, 0x500000, VMM_WRITABLE);
    uint8_t *page_b = vmm_alloc_user_page(b, 0x500000, VMM_WRITABLE);
    EXPECT_TRUE(page_a != NULL && page_b != NULL && page_a != page_b);
    page_a[0] = 0xAA;
    page_b[0] = 0xBB;
    EXPECT_EQ(page_a[0], 0xAA);
    EXPECT_TRUE(vmm_get_phys(a, 0x500000) != vmm_get_phys(b, 0x500000));

    // A user mapping never appears in the kernel's own address space
    EXPECT_TRUE(vmm_get_phys(NULL, 0x500000) == 0);

    // Both spaces see the same kernel half
    uint64_t *kernel = vmm_get_kernel_pml4();
    EXPECT_TRUE(a[256] == kernel[256] && a[511] == kernel[511] && b[400] == kernel[400]);

    // User pages are rejected above the user range
    EXPECT_TRUE(vmm_alloc_user_page(a, 0xFFFF900000000000ULL, VMM_WRITABLE) == NULL);

    vmm_destroy_address_space(a);
    vmm_destroy_address_space(b);
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

TEST(user_range_validation) {
    uint64_t *space = vmm_create_address_space();
    EXPECT_EQ(user_map_range(space, 0x600000, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE), 0);
    EXPECT_EQ(user_map_range(space, 0x700000, PAGE_SIZE, PROT_READ), 0);

    EXPECT_TRUE(vmm_user_range_ok(space, 0x600000, 2 * PAGE_SIZE, true));
    EXPECT_TRUE(vmm_user_range_ok(space, 0x600FFF, 2, false));           // Spans two mapped pages
    EXPECT_TRUE(!vmm_user_range_ok(space, 0x600000, 2 * PAGE_SIZE + 1, false)); // One byte too far
    EXPECT_TRUE(!vmm_user_range_ok(space, 0x5FFFFF, 2, false));          // Starts one byte early
    EXPECT_TRUE(vmm_user_range_ok(space, 0x700000, PAGE_SIZE, false));
    EXPECT_TRUE(!vmm_user_range_ok(space, 0x700000, PAGE_SIZE, true));   // Read-only
    EXPECT_TRUE(!vmm_user_range_ok(space, 0xFFFFFFFF80000000ULL, 1, false)); // Kernel address
    EXPECT_TRUE(!vmm_user_range_ok(space, 0x600000, (size_t)-1, false));     // Length wraps
    EXPECT_TRUE(!vmm_user_range_ok(space, USER_SPACE_END - 1, 2, false));    // Crosses into non-canonical
    EXPECT_TRUE(vmm_user_range_ok(space, 0x123456, 0, true));            // Empty range is fine

    vmm_destroy_address_space(space);
}

TEST(mmap_brk_and_unmap) {
    size_t free_before = pmm_get_free_frames();
    uint64_t *space = vmm_create_address_space();
    size_t free_with_space = pmm_get_free_frames();

    // mmap: zeroed pages at increasing addresses
    uint64_t cursor = USER_MMAP_BASE;
    uint64_t first = user_mmap_anon(space, &cursor, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE);
    uint64_t second = user_mmap_anon(space, &cursor, 1, PROT_READ);
    EXPECT_TRUE(first == USER_MMAP_BASE);
    EXPECT_TRUE(second == USER_MMAP_BASE + 3 * PAGE_SIZE);
    EXPECT_TRUE(cursor == USER_MMAP_BASE + 4 * PAGE_SIZE);

    uint64_t pte = vmm_get_pte(space, first);
    EXPECT_TRUE((pte & VMM_PRESENT) && (pte & VMM_USER) && (pte & VMM_WRITABLE) && (pte & VMM_NO_EXECUTE));
    EXPECT_TRUE(!(vmm_get_pte(space, second) & VMM_WRITABLE));

    const uint8_t *zeroed = (const uint8_t *)(vmm_get_phys(space, first) + pmm_get_hhdm_offset());
    EXPECT_TRUE(zeroed[0] == 0 && zeroed[PAGE_SIZE - 1] == 0);

    // Unmapping gives the frames back
    size_t free_mapped = pmm_get_free_frames();
    EXPECT_EQ(user_unmap_range(space, first, 3 * PAGE_SIZE), 0);
    EXPECT_TRUE(pmm_get_free_frames() == free_mapped + 3);
    EXPECT_TRUE(vmm_get_phys(space, first) == 0);
    EXPECT_TRUE(vmm_get_phys(space, second) != 0);

    // Bad ranges are refused
    EXPECT_EQ(user_unmap_range(space, first + 1, PAGE_SIZE), -1);            // Not page aligned
    EXPECT_EQ(user_map_range(space, 0xFFFF900000000000ULL, PAGE_SIZE, PROT_READ), -1);
    EXPECT_EQ(user_map_range(space, 0, PAGE_SIZE, PROT_READ), -1);           // NULL page
    EXPECT_TRUE(user_mmap_anon(space, &cursor, 0, PROT_READ) == 0);

    // brk: grows and shrinks by whole pages as the break moves
    uint64_t brk_start = 0x800000, brk_end = 0x800000;
    size_t free_brk = pmm_get_free_frames();
    EXPECT_TRUE(user_set_brk(space, brk_start, &brk_end, brk_start + 100) == brk_start + 100);
    EXPECT_TRUE(vmm_get_phys(space, brk_start) != 0);
    EXPECT_TRUE(user_set_brk(space, brk_start, &brk_end, brk_start + 3 * PAGE_SIZE) == brk_start + 3 * PAGE_SIZE);
    EXPECT_TRUE(vmm_get_phys(space, brk_start + 2 * PAGE_SIZE) != 0);
    EXPECT_TRUE(user_set_brk(space, brk_start, &brk_end, brk_start) == brk_start);
    EXPECT_TRUE(vmm_get_phys(space, brk_start) == 0);
    EXPECT_TRUE(pmm_get_free_frames() >= free_brk - 3);     // Only page tables remain

    // Below the start or absurdly far: break stays put
    EXPECT_TRUE(user_set_brk(space, brk_start, &brk_end, brk_start - 1) == brk_start);
    EXPECT_TRUE(user_set_brk(space, brk_start, &brk_end, brk_start + USER_BRK_MAX + 1) == brk_start);

    vmm_destroy_address_space(space);
    EXPECT_TRUE(free_with_space == free_before - 1);
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

TEST(frame_sharing_counts) {
    void *frame = pmm_alloc_frame();
    size_t free_after_alloc = pmm_get_free_frames();

    EXPECT_EQ(pmm_get_frame_ref(frame), 1u);
    pmm_ref_frame(frame);                     // A second owner
    EXPECT_EQ(pmm_get_frame_ref(frame), 2u);

    pmm_unref_frame(frame);                   // First owner leaves: still allocated
    EXPECT_TRUE(pmm_get_free_frames() == free_after_alloc);
    EXPECT_EQ(pmm_get_frame_ref(frame), 1u);

    pmm_unref_frame(frame);                   // Last owner leaves: freed
    EXPECT_TRUE(pmm_get_free_frames() == free_after_alloc + 1);
}

TEST(copy_on_write_clone) {
    size_t free_before = pmm_get_free_frames();
    uint64_t hhdm = pmm_get_hhdm_offset();

    uint64_t *parent = vmm_create_address_space();
    uint64_t *child = vmm_create_address_space();
    const uint64_t data_va = 0x500000, text_va = 0x501000; // Same page table

    uint8_t *data = vmm_alloc_user_page(parent, data_va, VMM_WRITABLE | VMM_NO_EXECUTE);
    EXPECT_TRUE(vmm_alloc_user_page(parent, text_va, 0) != NULL);   // Read-only, like program text
    data[0] = 0x11;
    uint64_t data_phys = vmm_get_phys(parent, data_va);
    uint64_t text_phys = vmm_get_phys(parent, text_va);

    size_t free_before_clone = pmm_get_free_frames();
    EXPECT_TRUE(vmm_clone_user_space(child, parent));

    // Cloning copies no page: the child only needs a PDPT, a PD and a PT
    EXPECT_TRUE(free_before_clone - pmm_get_free_frames() == 3);
    EXPECT_TRUE(vmm_get_phys(child, data_va) == data_phys);
    EXPECT_TRUE(vmm_get_phys(child, text_va) == text_phys);
    EXPECT_EQ(pmm_get_frame_ref((void *)data_phys), 2u);

    // The writable page is now read-only + COW on both sides; flags survive
    uint64_t parent_pte = vmm_get_pte(parent, data_va);
    uint64_t child_pte = vmm_get_pte(child, data_va);
    EXPECT_TRUE(!(parent_pte & VMM_WRITABLE) && (parent_pte & VMM_COW));
    EXPECT_TRUE(!(child_pte & VMM_WRITABLE) && (child_pte & VMM_COW));
    EXPECT_TRUE((child_pte & VMM_USER) && (child_pte & VMM_NO_EXECUTE));
    // The read-only page is shared but is not COW: writing it stays an error
    EXPECT_TRUE(!(vmm_get_pte(child, text_va) & (VMM_COW | VMM_WRITABLE)));
    EXPECT_TRUE(!vmm_handle_cow_fault(child, text_va));
    // A COW page still counts as writable memory for syscall arguments
    EXPECT_TRUE(vmm_user_range_ok(child, data_va, 8, true));
    EXPECT_TRUE(!vmm_user_range_ok(child, text_va, 8, true));

    // Child writes first: it gets a private copy with the same contents
    EXPECT_TRUE(vmm_handle_cow_fault(child, data_va + 123));
    uint64_t child_phys = vmm_get_phys(child, data_va);
    EXPECT_TRUE(child_phys != data_phys);
    EXPECT_EQ(((uint8_t *)(child_phys + hhdm))[0], 0x11);
    EXPECT_TRUE(vmm_get_pte(child, data_va) & VMM_WRITABLE);
    EXPECT_TRUE(!(vmm_get_pte(child, data_va) & VMM_COW));
    EXPECT_EQ(pmm_get_frame_ref((void *)data_phys), 1u);
    ((uint8_t *)(child_phys + hhdm))[0] = 0x22;
    EXPECT_EQ(data[0], 0x11);

    // Parent writes next: it is the last owner, so no copy, just write access
    size_t free_before_fault = pmm_get_free_frames();
    EXPECT_TRUE(vmm_handle_cow_fault(parent, data_va));
    EXPECT_TRUE(vmm_get_phys(parent, data_va) == data_phys);
    EXPECT_TRUE(vmm_get_pte(parent, data_va) & VMM_WRITABLE);
    EXPECT_TRUE(pmm_get_free_frames() == free_before_fault);

    // Not COW any more, unmapped, and kernel addresses: all refused
    EXPECT_TRUE(!vmm_handle_cow_fault(parent, data_va));
    EXPECT_TRUE(!vmm_handle_cow_fault(parent, 0x900000));
    EXPECT_TRUE(!vmm_handle_cow_fault(parent, 0xFFFFFFFF80000000ULL));

    // Destroying one space leaves the other's shared pages alive
    vmm_destroy_address_space(parent);
    EXPECT_TRUE(vmm_get_phys(child, text_va) == text_phys);
    EXPECT_EQ(pmm_get_frame_ref((void *)text_phys), 1u);
    vmm_destroy_address_space(child);
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

void test_suite_mmap(void) {
    kprintf("Suite: Address Spaces, mmap and brk\n");
    RUN_TEST(address_space_is_isolated_and_freed);
    RUN_TEST(user_range_validation);
    RUN_TEST(mmap_brk_and_unmap);
    RUN_TEST(frame_sharing_counts);
    RUN_TEST(copy_on_write_clone);
}
