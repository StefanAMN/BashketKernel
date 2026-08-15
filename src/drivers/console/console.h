/**
 * @file console.h
 * @brief Text on the framebuffer: an 8 x 8 font, ANSI colours and cursor
 *        movement, scrolling, and a cursor
 *
 * Everything kprintf and the terminal print ends up here (and on the serial
 * port). Once the memory manager runs, the console keeps a copy of the
 * screen in RAM: reading video memory back is slow, and the copy is what is
 * put back after a program has drawn on the screen itself (/dev/fb0).
 * Assumes 32 bits per pixel, which is what Limine sets up.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include "limine.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Start drawing on `fb` (the bootloader's framebuffer), from the top
 *        left, in white on black. First thing at boot: prints work at once.
 */
void console_init(struct limine_framebuffer *fb);

/**
 * @brief Keep a copy of the screen in RAM from now on (taken from video
 *        memory as it is). Needs the frame allocator. Without it scrolling
 *        is slow and /dev/fb0 cannot be written.
 */
void console_enable_backbuffer(void);

/**
 * @brief Print one character, which may be part of an ANSI escape
 *        sequence. Safe from any CPU and from interrupt handlers.
 */
void console_putchar(char c);

/**
 * @brief Print a NUL-terminated string (console_putchar for each).
 */
void console_write(const char *str);

/**
 * @brief Drop the console's lock whoever holds it. Only for fatal paths
 *        that must print.
 */
void console_force_unlock(void);

/**
 * @brief Let scrolling update only the in-RAM copy of the screen and leave
 *        it to console_refresh to show the result. Turn on once something
 *        (the timer) calls console_refresh regularly.
 */
void console_set_deferred_refresh(bool enabled);

/**
 * @brief Send the picture to video memory if scrolling left the screen out
 *        of date, and draw the cursor. Cheap when nothing changed. Also
 *        call it before halting, so the last messages are visible.
 */
void console_refresh(void);

/**
 * @brief Is there output, or a cursor, that console_refresh would draw?
 */
bool console_needs_refresh(void);

/**
 * @brief Give the screen to a program that draws on it directly. Calls
 *        nest. While it is given away console output still reaches the
 *        serial port and the console's own copy of the screen, only not
 *        video memory.
 * @return false if there is no framebuffer to give (or no RAM copy to put
 *         back afterwards).
 */
bool console_graphics_begin(void);

/**
 * @brief Take the screen back from the last program that had it,
 *        repainting the text.
 */
void console_graphics_end(void);

/**
 * @brief Whether a program has the screen (console_graphics_begin).
 */
bool console_graphics_active(void);

/**
 * @brief The framebuffer as the bootloader described it: where it is
 *        (kernel address), and its geometry. 32 bits per pixel. Any of the
 *        pointers may be NULL.
 * @return false if there is none (or it is not 32 bits per pixel).
 */
bool console_framebuffer(uint8_t **address, uint32_t *width, uint32_t *height, uint32_t *pitch);

/**
 * @brief For tests and diagnostics: where the next character will go, in
 *        character cells from 0.
 */
void console_get_cursor(uint32_t *column, uint32_t *row);

/**
 * @brief Size of the screen in character cells (80 x 25 without a
 *        framebuffer).
 */
void console_get_size(uint32_t *columns, uint32_t *rows);

/**
 * @brief For tests: the colour (0x00RRGGBB) of a pixel as last drawn, from
 *        the RAM copy (0 without one, or off the screen).
 */
uint32_t console_get_pixel(uint32_t x, uint32_t y);

#endif
