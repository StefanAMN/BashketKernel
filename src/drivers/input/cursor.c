/**
 * @file cursor.c
 * @brief Implementation of Graphical Mouse Pointer Rendering
 */

#include "cursor.h"
#include "kprintf.h"

static int32_t cursor_x = 640;
static int32_t cursor_y = 400;

void cursor_init(void) {
    kprintf("Cursor Init: Graphical mouse cursor subsystem ready (Pos: %d, %d)\n", cursor_x, cursor_y);
}

void cursor_update(int32_t x, int32_t y) {
    cursor_x = x;
    cursor_y = y;
}
