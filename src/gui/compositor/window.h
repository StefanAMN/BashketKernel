/**
 * @file window.h
 * @brief Graphical Desktop Window Manager & Compositor Interface
 */

#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    char title[64];
    uint32_t *buffer;
} window_t;

/**
 * @brief Allocate a new desktop GUI window structure and canvas buffer.
 * @param title Window header title.
 * @param x Initial screen X position.
 * @param y Initial screen Y position.
 * @param width Window canvas pixel width.
 * @param height Window canvas pixel height.
 * @return Pointer to created window_t object.
 */
window_t *window_create(const char *title, int32_t x, int32_t y, uint32_t width, uint32_t height);

/**
 * @brief Free desktop GUI window resources.
 * @param win Pointer to target window.
 */
void window_destroy(window_t *win);

#endif // WINDOW_H
