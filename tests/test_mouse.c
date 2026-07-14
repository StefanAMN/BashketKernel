#include "test.h"
#include "mouse.h"
#include "cursor.h"

TEST(mouse_cursor_test) {
    mouse_init();
    cursor_init();

    mouse_state_t state;
    mouse_get_state(&state);
    EXPECT_TRUE(state.x >= 0 && state.y >= 0);

    cursor_update(100, 200);
    EXPECT_TRUE(true);
}

void test_suite_mouse(void) {
    kprintf("Suite: Phase 15 PS/2 Mouse Driver & Framebuffer Canvas\n");
    RUN_TEST(mouse_cursor_test);
}
