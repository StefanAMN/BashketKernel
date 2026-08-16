#include "test.h"
#include "ansi.h"

TEST(ansi_color_decoding_test) {
    uint32_t red = ansi_get_color(31);
    EXPECT_TRUE(red == ANSI_COLOR_RED);

    uint32_t green = ansi_get_color(32);
    EXPECT_TRUE(green == ANSI_COLOR_GREEN);
}

void test_suite_ansi(void) {
    kprintf("Suite: Phase 20 Terminal ANSI Color Decoder\n");
    RUN_TEST(ansi_color_decoding_test);
}
