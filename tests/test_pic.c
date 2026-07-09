#include "../src/test.h"
#include "../src/pic.h"
#include "../src/io.h"

#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

TEST(remap_masks_all_lines) {
    pic_remap();
    // The data port doubles as the interrupt-mask register once
    // initialization (ICW1-4) is complete -- reading it back is a genuine
    // hardware-verified assertion, not just checking our own memory.
    EXPECT_EQ(inb(PIC1_DATA), 0xFF);
    EXPECT_EQ(inb(PIC2_DATA), 0xFF);
}

void test_suite_pic(void) {
    kprintf("Suite: PIC\n");
    RUN_TEST(remap_masks_all_lines);
}
