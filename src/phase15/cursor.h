/**
 * @file cursor.h
 * @brief Graphical Mouse Cursor Renderer Interface
 */

#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>

/**
 * @brief Initialize graphical cursor rendering subsystem.
 */
void cursor_init(void);

/**
 * @brief Update cursor position on screen.
 * @param x New X coordinate.
 * @param y New Y coordinate.
 */
void cursor_update(int32_t x, int32_t y);

#endif // CURSOR_H
