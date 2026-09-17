#include "test.h"
#include "virtio_pci.h"
#include "virtio_blk.h"
#include "vmm.h"
#include "pmm.h"
#include "kstring.h"
#include "interrupts.h"

// A device's 256 bytes of PCI configuration space, as the tests want it
static uint8_t space[256];

static uint8_t fake8(void *ctx, uint8_t offset) { (void)ctx; return space[offset]; }
// A configuration space the tests write (`space`), read a byte or 32 bits at a time
static uint32_t fake32(void *ctx, uint8_t offset) {
    (void)ctx;
    uint32_t value = 0;
    for (int i = 0; i < 4 && offset + i < 256; i++) value |= (uint32_t)space[offset + i] << (8 * i);
    return value;
}

// Store 32 bits in the fake configuration space
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

// An MSI-X capability at 0x98 (where sound_layout links one): a table of
// `entries` in BAR `bar` at `offset`
static void put_msix(uint16_t entries, uint8_t bar, uint32_t offset) {
    space[0x98] = 0x11;
    space[0x99] = 0x84;
    space[0x9A] = (uint8_t)((entries - 1) & 0xFF);
    space[0x9B] = (uint8_t)(((entries - 1) >> 8) & 0x07);
    put32(0x9C, offset | bar);
    put32(0xA0, 0x800 | bar);                           // Pending bits: not used here
}

TEST(msix_capability_is_found_and_not_believed) {
    virtio_msix_cap_t cap;
    sound_layout();
    put_msix(3, 1, 0);
    EXPECT_TRUE(virtio_pci_parse_msix(fake8, fake32, NULL, &cap));
    EXPECT_TRUE(cap.offset == 0x98 && cap.entries == 3 && cap.bar == 1 && cap.table_offset == 0);

    put_msix(2048, 4, 0x2000);                           // The largest table there can be
    EXPECT_TRUE(virtio_pci_parse_msix(fake8, fake32, NULL, &cap));
    EXPECT_TRUE(cap.entries == 2048 && cap.bar == 4 && cap.table_offset == 0x2000);

    put_msix(3, 6, 0);                                   // BIR 6: reserved
    EXPECT_TRUE(!virtio_pci_parse_msix(fake8, fake32, NULL, &cap));
    put_msix(3, 7, 0);
    EXPECT_TRUE(!virtio_pci_parse_msix(fake8, fake32, NULL, &cap));
    put_msix(16, 1, 0xFFFFFFF8u);                        // The table would wrap around
    EXPECT_TRUE(!virtio_pci_parse_msix(fake8, fake32, NULL, &cap));

    sound_layout();
    space[0x98] = 0x05;                                  // MSI, not MSI-X: nothing found
    EXPECT_TRUE(!virtio_pci_parse_msix(fake8, fake32, NULL, &cap));
    sound_layout();
    space[0x06] = 0;                                     // No capability list
    EXPECT_TRUE(!virtio_pci_parse_msix(fake8, fake32, NULL, &cap));
    memset(space, 0xFF, sizeof(space));                  // No device at all
    EXPECT_TRUE(!virtio_pci_parse_msix(fake8, fake32, NULL, &cap));
}

// The message a device writes to interrupt a CPU (Intel SDM Vol. 3A,
// 11.11): address 0xFEE with the APIC id in bits 19:12, data the vector,
// fixed delivery, edge triggered
TEST(msi_messages_name_the_cpu_and_vector) {
    uint64_t address = 0;
    uint32_t data = 0;
    EXPECT_TRUE(msi_message(0, 0x30, &address, &data));
    EXPECT_TRUE(address == 0xFEE00000ULL && data == 0x30);
    EXPECT_TRUE(msi_message(3, 0x3F, &address, &data));
    EXPECT_TRUE(address == 0xFEE03000ULL && data == 0x3F);
    EXPECT_TRUE(msi_message(0xFF, 0x31, &address, &data));
    EXPECT_TRUE(address == 0xFEEFF000ULL);
    EXPECT_TRUE(!msi_message(0x100, 0x30, &address, &data)); // Needs x2APIC
}

// A disk on MSI-X: a request is answered by its vector, on the CPU it was
// sent to, and its line stays quiet. On the line (VIRTIO=legacy), by the
// line. (Which of the two is expected is checked from the boot log: see
// the Makefile's VIRTIO_UNWANTED.)
TEST(disk_interrupts_arrive_by_their_route) {
    if (virtio_blk_count() == 0) return;
    uint8_t vector = 0, line = 0;
    uint32_t cpu = 0;
    bool msix = virtio_blk_interrupt_route(0, &vector, &cpu, &line);
    uint64_t on_cpu = msix ? msi_vector_count(vector, cpu) : 0;
    uint64_t on_line = irq_line_count(line);
    uint64_t taken = virtio_blk_interrupts(0);

    static uint8_t sector[512];
    for (int i = 0; i < 8; i++) EXPECT_EQ(virtio_blk_read_dev(0, (uint64_t)i, sector, 1), 0);

    EXPECT_TRUE(virtio_blk_interrupts(0) > taken);
    if (msix) {
        EXPECT_TRUE(vector >= MSI_VECTOR_FIRST && vector < MSI_VECTOR_FIRST + MSI_VECTOR_COUNT);
        EXPECT_TRUE(msi_vector_count(vector, cpu) > on_cpu);
        EXPECT_EQ(irq_line_count(line), on_line);
    } else {
        EXPECT_TRUE(irq_line_count(line) > on_line);
    }
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
    RUN_TEST(msix_capability_is_found_and_not_believed);
    RUN_TEST(msi_messages_name_the_cpu_and_vector);
    RUN_TEST(disk_interrupts_arrive_by_their_route);
}
