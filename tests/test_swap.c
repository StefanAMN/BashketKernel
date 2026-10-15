#include "test.h"
#include "swap.h"
#include "vmm.h"
#include "pmm.h"
#include "mmap.h"
#include "kstring.h"

#define BASE 0x0000000040000000ULL   // Where the tests' pages live in their address space
#define PAGES 24

// An address space of the tests' own, in use on no CPU: pages can be taken
// from it and brought back without a process being involved
static uint64_t *make_space(void) {
    uint64_t *space = vmm_create_address_space();
    if (!space) return NULL;
    for (uint64_t i = 0; i < PAGES; i++) {
        uint64_t *view = vmm_alloc_user_page(space, BASE + i * PAGE_SIZE, VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE);
        if (!view) return NULL;
        for (int w = 0; w < 512; w++) view[w] = (i << 32) | (uint64_t)w;
    }
    return space;
}

// Is page `i` of the space there, with what make_space put in it?
static bool page_is_whole(uint64_t *space, uint64_t i) {
    uint64_t phys = vmm_get_phys(space, BASE + i * PAGE_SIZE);
    if (!phys) return false;
    const uint64_t *view = (const uint64_t *)(phys + pmm_get_hhdm_offset());
    for (int w = 0; w < 512; w++) if (view[w] != ((i << 32) | (uint64_t)w)) return false;
    return true;
}

static size_t swapped_count(uint64_t *space) {
    size_t n = 0;
    for (uint64_t i = 0; i < PAGES; i++) if (vmm_pte_swapped(vmm_get_pte(space, BASE + i * PAGE_SIZE))) n++;
    return n;
}

TEST(swap_header_is_checked) {
    static uint8_t page[PAGE_SIZE];
    uint32_t slots = 0;
    memset(page, 0, sizeof(page));
    memcpy(page + PAGE_SIZE - 10, "SWAPSPACE2", 10);
    page[1024] = 1;                                   // Version
    page[1028] = 0xFF; page[1029] = 0x0F;             // Last page: 4095

    EXPECT_TRUE(swap_parse_header(page, 4096, &slots));
    EXPECT_EQ(slots, 4095u);

    EXPECT_TRUE(!swap_parse_header(page, 4095, &slots));     // The disk is a page shorter than the header says
    EXPECT_TRUE(!swap_parse_header(page, 0, &slots));
    page[1024] = 2;
    EXPECT_TRUE(!swap_parse_header(page, 4096, &slots));     // A version we do not know
    page[1024] = 1;
    page[1032] = 1;
    EXPECT_TRUE(!swap_parse_header(page, 4096, &slots));     // Bad pages listed
    page[1032] = 0;
    page[1028] = 0; page[1029] = 0;
    EXPECT_TRUE(!swap_parse_header(page, 4096, &slots));     // No pages at all
    page[1028] = 0xFF; page[1029] = 0x0F;
    page[PAGE_SIZE - 1] = '1';                               // "SWAPSPACE1": not ours
    EXPECT_TRUE(!swap_parse_header(page, 4096, &slots));
    EXPECT_TRUE(!swap_parse_header(NULL, 4096, &slots));

    // The disk mkswap made on the host was accepted at boot
    EXPECT_TRUE(swap_active());
    EXPECT_TRUE(swap_total_pages() >= 1024 && swap_free_pages() == swap_total_pages());
}

TEST(pages_go_out_and_come_back) {
    size_t frames_before = test_free_frames(), slots_before = swap_free_pages();
    uint64_t out_before = swap_pages_out(), in_before = swap_pages_in();
    uint64_t *space = make_space();
    EXPECT_TRUE(space != NULL);
    if (!space) return;
    size_t frames_with_space = test_free_frames();

    // Ten pages out: ten frames back, ten slots taken, ten entries that are
    // not present but say where the page is
    uint64_t cursor = 0;
    EXPECT_EQ(swap_out(space, &cursor, 10), (size_t)10);
    EXPECT_EQ(swapped_count(space), (size_t)10);
    EXPECT_TRUE(test_free_frames() == frames_with_space + 10);
    EXPECT_TRUE(swap_free_pages() == slots_before - 10);
    EXPECT_TRUE(swap_pages_out() == out_before + 10);
    EXPECT_TRUE(cursor > BASE);

    // A swapped entry keeps the page's permissions and is not "present"
    uint64_t entry = 0;
    for (uint64_t i = 0; i < PAGES && !vmm_pte_swapped(entry); i++) entry = vmm_get_pte(space, BASE + i * PAGE_SIZE);
    EXPECT_TRUE(vmm_pte_swapped(entry) && (entry & VMM_USER) && (entry & VMM_WRITABLE) && (entry & VMM_NO_EXECUTE));
    EXPECT_TRUE(vmm_pte_swap_slot(entry) >= 1 && vmm_pte_swap_slot(entry) <= swap_total_pages());

    // Every one comes back with what it held; a page that is present, or an
    // address with nothing behind it, is not the swap code's business
    for (uint64_t i = 0; i < PAGES; i++) {
        bool was_out = vmm_pte_swapped(vmm_get_pte(space, BASE + i * PAGE_SIZE));
        EXPECT_EQ(swap_in(space, BASE + i * PAGE_SIZE + 123), was_out ? 1 : 0);
        EXPECT_TRUE(page_is_whole(space, i));
    }
    EXPECT_EQ(swap_in(space, BASE + 500 * PAGE_SIZE), 0);
    EXPECT_EQ(swapped_count(space), (size_t)0);
    EXPECT_TRUE(swap_pages_in() == in_before + 10);
    EXPECT_TRUE(swap_free_pages() == slots_before);
    EXPECT_TRUE(test_free_frames() == frames_with_space);

    // All of them out, however many are asked for
    EXPECT_EQ(swap_out(space, &cursor, 1000), (size_t)PAGES);
    EXPECT_EQ(swap_out(space, &cursor, 1), (size_t)0);

    // Unmapping a swapped page gives its slot back; so does destroying the
    // address space with the rest still out
    EXPECT_EQ(user_unmap_range(space, BASE, 4 * PAGE_SIZE), 0);
    EXPECT_TRUE(swap_free_pages() == slots_before - (PAGES - 4));
    EXPECT_TRUE(vmm_get_pte(space, BASE) == 0);
    vmm_destroy_address_space(space);
    EXPECT_TRUE(swap_free_pages() == slots_before);
    EXPECT_TRUE(test_free_frames() == frames_before);
}

// The clock: a page used since the hand last passed is spared once
TEST(recently_used_pages_are_spared) {
    size_t frames_before = test_free_frames(), slots_before = swap_free_pages();
    uint64_t *space = make_space();
    EXPECT_TRUE(space != NULL);
    if (!space) return;

    // Nothing has touched these pages through their own address space, so
    // no accessed bit is set. Mark the first eight as used.
    for (uint64_t i = 0; i < 8; i++) {
        uint64_t virt = BASE + i * PAGE_SIZE, entry = vmm_get_pte(space, virt);
        EXPECT_TRUE(vmm_map_page(space, virt, entry & PTE_ADDR_MASK,
                                 (entry & (VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE)) | VMM_ACCESSED));
    }
    uint64_t cursor = 0;
    EXPECT_EQ(swap_out(space, &cursor, 8), (size_t)8);
    for (uint64_t i = 0; i < 8; i++) {
        uint64_t entry = vmm_get_pte(space, BASE + i * PAGE_SIZE);
        EXPECT_TRUE((entry & VMM_PRESENT) && !(entry & VMM_ACCESSED));   // Passed over, and the bit cleared
    }
    for (uint64_t i = 8; i < 16; i++) EXPECT_TRUE(vmm_pte_swapped(vmm_get_pte(space, BASE + i * PAGE_SIZE)));

    // When everything else has gone, the hand comes round to them too
    EXPECT_EQ(swap_out(space, &cursor, 1000), (size_t)(PAGES - 8));
    EXPECT_EQ(swapped_count(space), (size_t)PAGES);

    // A page that has come back is marked used: it is not the next to go
    EXPECT_EQ(swap_in(space, BASE), 1);
    EXPECT_TRUE(vmm_get_pte(space, BASE) & VMM_ACCESSED);

    vmm_destroy_address_space(space);
    EXPECT_TRUE(swap_free_pages() == slots_before && test_free_frames() == frames_before);
}

// fork: both address spaces refer to the one slot, each reads its own copy
// back, and the slot is free when both have. A frame two address spaces
// share is never taken.
TEST(swapped_pages_and_fork) {
    size_t frames_before = test_free_frames(), slots_before = swap_free_pages();
    uint64_t *parent = make_space();
    uint64_t *child = vmm_create_address_space();
    EXPECT_TRUE(parent != NULL && child != NULL);
    if (!parent || !child) return;

    uint64_t cursor = 0;
    EXPECT_EQ(swap_out(parent, &cursor, 6), (size_t)6);
    EXPECT_TRUE(vmm_clone_user_space(child, parent));
    EXPECT_EQ(swapped_count(child), (size_t)6);
    EXPECT_TRUE(swap_free_pages() == slots_before - 6);       // Shared, not copied

    // The pages that stayed are now shared between the two (copy-on-write):
    // neither may have them taken
    uint64_t child_cursor = 0;
    EXPECT_EQ(swap_out(parent, &cursor, 100), (size_t)0);
    EXPECT_EQ(swap_out(child, &child_cursor, 100), (size_t)0);

    // Each side reads its own copy; the slot goes when the second has
    uint64_t first = 0;
    while (first < PAGES && !vmm_pte_swapped(vmm_get_pte(parent, BASE + first * PAGE_SIZE))) first++;
    EXPECT_EQ(swap_in(child, BASE + first * PAGE_SIZE), 1);
    EXPECT_TRUE(page_is_whole(child, first));
    EXPECT_TRUE(swap_free_pages() == slots_before - 6);       // The parent still needs it
    EXPECT_EQ(swap_in(parent, BASE + first * PAGE_SIZE), 1);
    EXPECT_TRUE(page_is_whole(parent, first));
    EXPECT_TRUE(swap_free_pages() == slots_before - 5);
    EXPECT_TRUE(vmm_get_phys(parent, BASE + first * PAGE_SIZE) != vmm_get_phys(child, BASE + first * PAGE_SIZE));

    // One side goes away with pages still out: the other still has them
    vmm_destroy_address_space(child);
    EXPECT_TRUE(swap_free_pages() == slots_before - 5);
    for (uint64_t i = 0; i < PAGES; i++) {
        swap_in(parent, BASE + i * PAGE_SIZE);
        EXPECT_TRUE(page_is_whole(parent, i));
    }
    vmm_destroy_address_space(parent);
    EXPECT_TRUE(swap_free_pages() == slots_before && test_free_frames() == frames_before);
}

void test_suite_swap(void) {
    kprintf("Suite: Swap\n");
    if (!swap_active()) {
        kprintf("  (no swap disk attached: skipped)\n");
        return;
    }
    RUN_TEST(swap_header_is_checked);
    RUN_TEST(pages_go_out_and_come_back);
    RUN_TEST(recently_used_pages_are_spared);
    RUN_TEST(swapped_pages_and_fork);
}
