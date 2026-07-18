/**
 * @file ansi.c
 * @brief Implementation of Terminal ANSI Color Palette Decoding
 */

#include "ansi.h"

uint32_t ansi_get_color(int code) {
    switch (code) {
        case 30: return ANSI_COLOR_BLACK;
        case 31: return ANSI_COLOR_RED;
        case 32: return ANSI_COLOR_GREEN;
        case 33: return ANSI_COLOR_YELLOW;
        case 34: return ANSI_COLOR_BLUE;
        case 35: return ANSI_COLOR_MAGENTA;
        case 36: return ANSI_COLOR_CYAN;
        case 37: return ANSI_COLOR_WHITE;
        default: return ANSI_COLOR_WHITE;
    }
}
