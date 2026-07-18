/**
 * @file ansi.h
 * @brief Terminal ANSI Color Palette Decoder Interface
 */

#ifndef ANSI_H
#define ANSI_H

#include <stdint.h>

#define ANSI_COLOR_BLACK   0x000000
#define ANSI_COLOR_RED     0xFF0000
#define ANSI_COLOR_GREEN   0x00FF00
#define ANSI_COLOR_YELLOW  0xFFFF00
#define ANSI_COLOR_BLUE    0x0000FF
#define ANSI_COLOR_MAGENTA 0xFF00FF
#define ANSI_COLOR_CYAN    0x00FFFF
#define ANSI_COLOR_WHITE   0xFFFFFF

/**
 * @brief Convert 8-bit ANSI color code (30..37) into 32-bit RGB pixel color.
 * @param code ANSI color index (0 for default white).
 * @return 32-bit 0x00RRGGBB hex color value.
 */
uint32_t ansi_get_color(int code);

#endif // ANSI_H
