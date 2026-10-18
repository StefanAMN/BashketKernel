#include "test.h"
#include "lapic.h"
#include "ioapic.h"
#include "acpi.h"
#include "ext2.h"
#include "bcache.h"

TEST(apic_initialization_test) {
    uint32_t id = lapic_get_id();
    EXPECT_TRUE(id == 0); // Primary CPU APIC ID 0
}

// \_S5 in AML bytecode, in the forms firmware writes it. (The bytes come from
// the firmware, so the parser is also given ones that are wrong or cut short.)
TEST(acpi_finds_the_sleep_type_for_off) {
    uint8_t a = 9, b = 9;
    // Name (_S5, Package (0x04) { 0x05, 0x05, Zero, Zero }): BytePrefix values
    static const uint8_t bytes[] = { 0x10, 0x08, '_', 'S', '5', '_', 0x12, 0x0A, 0x04, 0x0A, 0x05, 0x0A, 0x05, 0x00, 0x00 };
    EXPECT_TRUE(acpi_parse_s5(bytes, sizeof(bytes), &a, &b));
    EXPECT_TRUE(a == 5 && b == 5);
    // Name (\_S5, Package { Zero, Zero }) with the root prefix (as QEMU's)
    static const uint8_t rooted[] = { 0x08, '\\', '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x00, 0x00 };
    EXPECT_TRUE(acpi_parse_s5(rooted, sizeof(rooted), &a, &b));
    EXPECT_TRUE(a == 0 && b == 0);
    // A two-byte package length, and One for the second value
    static const uint8_t longer[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x46, 0x00, 0x02, 0x0A, 0x07, 0x01 };
    EXPECT_TRUE(acpi_parse_s5(longer, sizeof(longer), &a, &b));
    EXPECT_TRUE(a == 7 && b == 1);

    // "_S5_" that is not a name being defined (a reference to it, say)
    static const uint8_t mention[] = { 0x70, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x00, 0x00 };
    EXPECT_TRUE(!acpi_parse_s5(mention, sizeof(mention), &a, &b));
    // A sleep type that does not fit in three bits, an element of another
    // kind, and bytes that end in the middle
    static const uint8_t too_big[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 0x09, 0x00 };
    EXPECT_TRUE(!acpi_parse_s5(too_big, sizeof(too_big), &a, &b));
    static const uint8_t string[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0D, 'x', 0x00 };
    EXPECT_TRUE(!acpi_parse_s5(string, sizeof(string), &a, &b));
    static const uint8_t cut[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A };
    EXPECT_TRUE(!acpi_parse_s5(cut, sizeof(cut), &a, &b));
    EXPECT_TRUE(!acpi_parse_s5(NULL, 100, &a, &b));

    // QEMU's firmware describes it
    EXPECT_TRUE(acpi_can_power_off());
}

// While the system disk is in use it does not carry the clean mark: a crash
// now must lead to a check at the next mount
TEST(mounted_volume_is_not_marked_clean) {
    if (!ext2_mounted()) return;
    static uint8_t superblock[1024];
    bcache_invalidate();
    EXPECT_EQ(bcache_read_dev(1, 2, superblock, 2), 0);
    EXPECT_EQ(superblock[58] & 1, 0);                      // s_state, bit 0: unmounted cleanly
    EXPECT_TRUE(!ext2_volume_marked_clean());
}

void test_suite_apic(void) {
    kprintf("Suite: APIC, I/O APIC and ACPI\n");
    RUN_TEST(apic_initialization_test);
    RUN_TEST(acpi_finds_the_sleep_type_for_off);
    RUN_TEST(mounted_volume_is_not_marked_clean);
}
