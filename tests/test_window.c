#include "test.h"
#include "window.h"

TEST(window_creation_destruction_test) {
    window_t *win = window_create("Terminal Window", 50, 50, 640, 480);
    EXPECT_TRUE(win != NULL);
    EXPECT_TRUE(win->buffer != NULL);
    EXPECT_TRUE(win->width == 640 && win->height == 480);

    window_destroy(win);
}

void test_suite_window(void) {
    kprintf("Suite: Phase 30 Desktop Window Manager & GUI Compositor\n");
    RUN_TEST(window_creation_destruction_test);
}
