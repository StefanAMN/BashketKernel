#include "test.h"
#include "virtio_pci.h"
#include "virtio_blk.h"
#include "vmm.h"
#include "pmm.h"
#include "kstring.h"

// A device's 256 bytes of PCI configuration space, as the tests want it
static uint8_t space[256];

static uint8_t fake8(void *ctx, uint8_t offset) { (void)ctx; return space[offset]; }
static uint32_t fake32(void *ctx, uint8_t offset) {
    (void)ctx;
    uint32_t value = 0;
    for (int i = 0; i < 4 && offset + i < 256; i++) value |= (uint32_t)space[offset + i] << (8 * i);
    return value;
}

static void put32(uint8_t at, uint32_t value) {
    for (int i = 0; i < 4; i++) space[at + i] = (uint8_t)(value >> (8 * i));
}

// A virtio capability at `at`, linked to `next`
static void put_cap(uint8_t at, uint8_t next, uint8_t type, uint8_t bar, uint32_t offset, uint32_t length) {
    space[at] = 0x09;                        // Vendor-specific
    space[at + 1] = next;
    space[at + 2] = type == 2 ? 20 : 16;     // This entry's own length
    space[at + 3] = type;
    space[at + 4] = bar;
    put32((uint8_t)(at + 8), offset);
    put32((uint8_t)(at + 12), length);
    if (type == 2) put32((uint8_t)(at + 16), 4);
}

// What QEMU's devices have: four structures in BAR 4, a page each, after an
// unrelated capability (MSI-X, id 0x11)
static void sound_layout(void) {
    memset(space, 0, sizeof(space));
    space[0x06] = 0x10;                      // Status: has a capability list
    space[0x34] = 0x98;
    space[0x98] = 0x11; space[0x99] = 0x84;
    put_cap(0x84, 0x70, 5, 0, 0, 0);         // PCI configuration access: not one we use
    put_cap(0x70, 0x60, 2, 4, 0x3000, 0x1000);
    put_cap(0x60, 0x50, 4, 4, 0x2000, 0x1000);
    put_cap(0x50, 0x40, 3, 4, 0x1000, 0x1000);
    put_cap(0x40, 0x00, 1, 4, 0x0000, 0x1000);
}

TEST(virtio_capabilities_are_found) {
    virtio_caps_t caps;
    sound_layout();
    EXPECT_TRUE(virtio_pci_parse_caps(fake8, fake32, NULL, &caps));
    EXPECT_TRUE(caps.common.bar == 4 && caps.common.offset == 0 && caps.common.length == 0x1000);
    EXPECT_TRUE(caps.isr.offset == 0x1000 && caps.device.offset == 0x2000 && caps.notify.offset == 0x3000);
    EXPECT_EQ(caps.notify_multiplier, 4u);

    // Of two of a kind, the first in the list is the one to use
    put_cap(0x40, 0xB0, 1, 4, 0x0000, 0x1000);
    put_cap(0xB0, 0x00, 1, 2, 0x8000, 0x1000);
    EXPECT_TRUE(virtio_pci_parse_caps(fake8, fake32, NULL, &caps));
    EXPECT_TRUE(caps.common.bar == 4 && caps.common.offset == 0);
}

// The list is the device's to write, and a device can be wrong or hostile
TEST(virtio_capabilities_are_not_believed) {
    virtio_caps_t caps;

    sound_layout();
    space[0x06] = 0;                                     // No capability list at all: a legacy device
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    sound_layout();
    space[0x41] = 0x98;                                  // The last entry leads back to the first: a loop
    EXPECT_TRUE(virtio_pci_parse_caps(fake8, fake32, NULL, &caps)); // Found, and the walk ended

    sound_layout();
    space[0x99] = 0x98;                                  // An entry that names itself, before any of ours
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    sound_layout();
    space[0x51] = 0x00;                                  // The list ends before the common structure
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));
    EXPECT_TRUE(!caps.common.found && caps.isr.found);

    sound_layout();
    space[0x44] = 6;                                     // A BAR that does not exist
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    sound_layout();
    put32(0x4C, 40);                                     // Too short to hold the common registers
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    sound_layout();
    put32(0x4C, 0x80000000u);                            // Absurdly long
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    sound_layout();
    put32(0x48, 0xFFFFFFF0u);                            // Offset plus length wraps around
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    sound_layout();
    space[0x72] = 16;                                    // A notify entry too short to hold its multiplier
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    sound_layout();
    space[0x34] = 0x20;                                  // A list that starts inside the standard header
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));

    memset(space, 0xFF, sizeof(space));                  // No device there at all: everything reads as ones
    EXPECT_TRUE(!virtio_pci_parse_caps(fake8, fake32, NULL, &caps));
}

// Device registers get addresses of their own, uncached, with the offset
// into the page kept. An ordinary frame stands in for the device.
TEST(device_memory_is_mapped_in_its_own_region) {
    void *frames = pmm_alloc_frames(2);
    EXPECT_TRUE(frames != NULL);
    if (!frames) return;
    uint8_t *direct = (uint8_t *)((uint64_t)frames + pmm_get_hhdm_offset());
    memset(direct, 0, 2 * PAGE_SIZE);
    direct[0x123] = 0xAB;
    direct[PAGE_SIZE + 5] = 0xCD;

    // 0x1000 bytes from 0x123 on: needs both pages
    volatile uint8_t *mapped = vmm_map_device((uint64_t)frames + 0x123, 0x1000);
    EXPECT_TRUE(mapped != NULL);
    if (!mapped) return;
    EXPECT_TRUE((uint64_t)mapped >= DEVICE_REGION_BASE && ((uint64_t)mapped & 0xFFF) == 0x123);
    EXPECT_EQ(mapped[0], 0xAB);
    EXPECT_EQ(mapped[PAGE_SIZE + 5 - 0x123], 0xCD);
    mapped[1] = 0x77;                                    // Writes arrive
    EXPECT_EQ(direct[0x124], 0x77);

    // PCD and PWT set, not executable; and the page after it is not mapped
    uint64_t pte = vmm_get_pte(NULL, (uint64_t)mapped);
    EXPECT_TRUE((pte & (VMM_CACHE_DIS | VMM_WRITE_THRU | VMM_NO_EXECUTE)) == (VMM_CACHE_DIS | VMM_WRITE_THRU | VMM_NO_EXECUTE));
    EXPECT_TRUE(!(vmm_get_pte(NULL, ((uint64_t)mapped & ~0xFFFULL) + 2 * PAGE_SIZE) & VMM_PRESENT));

    // A second mapping does not land on the first
    volatile uint8_t *second = vmm_map_device((uint64_t)frames, 16);
    EXPECT_TRUE(second != NULL && (uint64_t)second >= ((uint64_t)mapped & ~0xFFFULL) + 3 * PAGE_SIZE);
    EXPECT_TRUE(vmm_map_device((uint64_t)frames, 0) == NULL);

    // (The mappings stay: device registers are mapped for good. The frames
    // go back; nothing reads through the mappings again.)
    vmm_unmap_page(NULL, (uint64_t)mapped & ~0xFFFULL);
    vmm_unmap_page(NULL, ((uint64_t)mapped & ~0xFFFULL) + PAGE_SIZE);
    vmm_unmap_page(NULL, (uint64_t)second & ~0xFFFULL);
    pmm_free_frames(frames, 2);
}

void test_suite_virtio(void) {
    kprintf("Suite: virtio transport\n");
    RUN_TEST(virtio_capabilities_are_found);
    RUN_TEST(virtio_capabilities_are_not_believed);
    RUN_TEST(device_memory_is_mapped_in_its_own_region);
}
