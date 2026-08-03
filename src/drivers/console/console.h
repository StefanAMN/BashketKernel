#ifndef CONSOLE_H
#define CONSOLE_H

#include "limine.h"
#include <stdint.h>
#include <stdbool.h>

void console_init(struct limine_framebuffer *fb);
void console_enable_backbuffer(void);
void console_putchar(char c);

// Drop the console's lock whoever holds it. Only for fatal paths that must print.
void console_force_unlock(void);

// Let scrolling update only the in-RAM copy of the screen and leave it to
// console_refresh to show the result. Turn on once something (the timer)
// calls console_refresh regularly.
void console_set_deferred_refresh(bool enabled);

// Send the picture to video memory if scrolling left the screen out of date.
// Cheap when nothing changed. Also call it before halting, so the last
// messages are visible.
void console_refresh(void);
void console_write(const char *str);

// For tests and diagnostics: where the next character will go (in character
// cells, from 0), and the colour (0x00RRGGBB) of a pixel as last drawn.
void console_get_cursor(uint32_t *column, uint32_t *row);

// Size of the screen in character cells
void console_get_size(uint32_t *columns, uint32_t *rows);
uint32_t console_get_pixel(uint32_t x, uint32_t y);

#endif
