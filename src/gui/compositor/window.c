/**
 * @file window.c
 * @brief Implementation of Desktop Window Manager Canvas
 */

#include "window.h"
#include "heap.h"
#include "string_util.h"

window_t *window_create(const char *title, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    window_t *win = (window_t *)kmalloc(sizeof(window_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(window_t));

    win->x = x;
    win->y = y;
    win->width = width;
    win->height = height;

    if (title) {
        size_t i = 0;
        for (; title[i] != '\0' && i < sizeof(win->title) - 1; i++) {
            win->title[i] = title[i];
        }
        win->title[i] = '\0';
    }

    size_t buf_size = width * height * sizeof(uint32_t);
    win->buffer = (uint32_t *)kmalloc(buf_size);
    if (win->buffer) {
        memset(win->buffer, 0x1E, buf_size); // Dark grey canvas
    }

    return win;
}

void window_destroy(window_t *win) {
    if (win) {
        if (win->buffer) {
            kfree(win->buffer);
        }
        kfree(win);
    }
}
