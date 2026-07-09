#include "../src/test.h"
#include "../src/gdt.h"

TEST(entry_size_is_8_bytes) {
    EXPECT_EQ(sizeof(struct gdt_entry), 8);
}

TEST(null_descriptor_is_zeroed) {
    EXPECT_EQ(gdt_entries[0].limit_low, 0);
    EXPECT_EQ(gdt_entries[0].base_low, 0);
    EXPECT_EQ(gdt_entries[0].base_mid, 0);
    EXPECT_EQ(gdt_entries[0].access, 0);
    EXPECT_EQ(gdt_entries[0].granularity, 0);
    EXPECT_EQ(gdt_entries[0].base_high, 0);
}

TEST(kernel_code_segment_is_correct) {
    // Present=1 DPL=0 S=1 Type=1010 (code, execute/read)
    EXPECT_EQ(gdt_entries[1].access, 0x9A);
    // Long-mode bit (bit 5 of granularity) must be set -- this is what
    // actually makes it a 64-bit code segment, not the limit/base.
    EXPECT_TRUE((gdt_entries[1].granularity & 0x20) != 0);
}

TEST(kernel_data_segment_is_correct) {
    // Present=1 DPL=0 S=1 Type=0010 (data, read/write)
    EXPECT_EQ(gdt_entries[2].access, 0x92);
}

void test_suite_gdt(void) {
    kprintf("Suite: GDT\n");
    RUN_TEST(entry_size_is_8_bytes);
    RUN_TEST(null_descriptor_is_zeroed);
    RUN_TEST(kernel_code_segment_is_correct);
    RUN_TEST(kernel_data_segment_is_correct);
}
