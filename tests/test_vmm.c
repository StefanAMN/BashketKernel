#include "test.h"
#include "vmm.h"
#include "pmm.h"

// A frame mapped at a virtual address can be written through it and then unmapped
TEST(vmm_mapping) {
    void *phys_frame = pmm_alloc_frame();
    EXPECT_TRUE(phys_frame != NULL);

    uint64_t virt_addr = 0xFFFFC00010000000ULL;
    bool mapped = vmm_map_page(NULL, virt_addr, (uint64_t)phys_frame, VMM_PRESENT | VMM_WRITABLE);
    EXPECT_TRUE(mapped);

    // Test writing and reading through mapped virtual address
    volatile uint32_t *ptr = (volatile uint32_t *)virt_addr;
    *ptr = 0x12345678;
    EXPECT_EQ(*ptr, 0x12345678);

    bool unmapped = vmm_unmap_page(NULL, virt_addr);
    EXPECT_TRUE(unmapped);

    pmm_free_frame(phys_frame);
}

void test_suite_vmm(void) {
    kprintf("Suite: VMM (Virtual Memory Manager & Paging)\n");
    RUN_TEST(vmm_mapping);
}
