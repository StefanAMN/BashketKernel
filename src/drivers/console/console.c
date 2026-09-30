#include "console.h"
#include "font.h"
#include "pmm.h"
#include "kstring.h"
#include "ansi.h"
#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>

static struct limine_framebuffer *g_fb = NULL;
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t *backbuffer = NULL;

// Deferred refresh. Scrolling changes every pixel on the screen, and sending
// the whole picture to video memory for each scrolled line made output crawl.
// Once the timer is running, a scroll only changes the RAM copy and marks the
// screen stale; console_refresh (called from the timer) sends the picture at
// most once per call. While the screen is stale, new characters are drawn in
// the RAM copy only, since the next refresh will show them.
static bool deferred_refresh = false;
static bool screen_stale = false;

#define DEFAULT_FG 15 // Bright white, as an index into the ANSI palette
#define DEFAULT_BG 0  // Black
#define CELL 8        // Characters are 8 x 8 pixels

// Current colours, changed by ANSI "SGR" sequences
static int fg_index = DEFAULT_FG;
static int bg_index = DEFAULT_BG;
static uint32_t FG_COLOR = 0x00FFFFFF;
static uint32_t BG_COLOR = 0x00000000;
static ansi_parser_t parser;

// The cursor is an underline in the cell where the next character will go.
// It is drawn by inverting pixels, so drawing it twice removes it. To keep
// output fast it is removed before any character is drawn and only put back
// by console_refresh, which the timer calls: during a burst of output it is
// simply absent.
static bool cursor_drawn = false;
static uint32_t cursor_drawn_x, cursor_drawn_y;

void console_init(struct limine_framebuffer *fb) {
    g_fb = fb;
    cursor_x = 0;
    cursor_y = 0;
    ansi_init(&parser);
    FG_COLOR = ansi_color(fg_index);
    BG_COLOR = ansi_color(bg_index);
}

void console_enable_backbuffer(void) {
    if (!g_fb || backbuffer) return;

    size_t pixels_per_row = g_fb->pitch / 4;
    size_t total_pixels = pixels_per_row * g_fb->height;
    size_t buffer_size = total_pixels * sizeof(uint32_t);
    size_t pages = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;

    void *phys = pmm_alloc_frames(pages);
    if (!phys) return;

    backbuffer = (uint32_t *)((uint64_t)phys + pmm_get_hhdm_offset());

    // Copy initial screen content from video memory into backbuffer
    uint32_t *fb_ptr = g_fb->address;
    for (size_t i = 0; i < total_pixels; i++) {
        backbuffer[i] = fb_ptr[i];
    }
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color);

static void invert_cursor_cell(uint32_t cell_x, uint32_t cell_y) {
    if (!backbuffer) return;
    for (uint32_t y = CELL - 2; y < CELL; y++) {         // The bottom two rows of the cell
        for (uint32_t x = 0; x < CELL; x++) {
            uint32_t px = cell_x + x, py = cell_y + y;
            if (px >= g_fb->width || py >= g_fb->height) continue;
            uint32_t current = backbuffer[py * (g_fb->pitch / 4) + px];
            putpixel(px, py, current ^ 0x00FFFFFF);
        }
    }
}

static void hide_cursor(void) {
    if (!cursor_drawn) return;
    invert_cursor_cell(cursor_drawn_x, cursor_drawn_y);
    cursor_drawn = false;
}

static void show_cursor(void) {
    if (cursor_drawn || !backbuffer) return;
    cursor_drawn_x = cursor_x;
    cursor_drawn_y = cursor_y;
    invert_cursor_cell(cursor_drawn_x, cursor_drawn_y);
    cursor_drawn = true;
}

void console_set_deferred_refresh(bool enabled) {
    deferred_refresh = enabled && backbuffer != NULL;
    if (!deferred_refresh) console_refresh();
}

void console_refresh(void) {
    if (!g_fb || !backbuffer) return;

    uint64_t flags = irq_save();
    if (screen_stale) {
        size_t total_pixels = (size_t)(g_fb->pitch / 4) * g_fb->height;
        screen_stale = false; // Cleared first: anything drawn from here on goes to the screen too
        memcpy(g_fb->address, backbuffer, total_pixels * sizeof(uint32_t));
    }
    if (deferred_refresh) show_cursor(); // Output has paused for at least a tick: show where we are
    irq_restore(flags);
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb->width || y >= g_fb->height) return;
    uint32_t idx = y * (g_fb->pitch / 4) + x;
    if (backbuffer) {
        backbuffer[idx] = color;
        if (screen_stale) return; // The next refresh will show it
    }
    uint32_t *fb_ptr = g_fb->address;
    fb_ptr[idx] = color;
}

static void scroll(void) {
    uint32_t *fb_ptr = g_fb->address;
    uint32_t pixels_per_row = g_fb->pitch / 4;
    uint32_t total_pixels = pixels_per_row * g_fb->height;
    uint32_t offset = pixels_per_row * 8; // 8 rows of pixels
    
    if (backbuffer) {
        // Shift the text up one line in the RAM copy of the screen
        memmove(backbuffer, backbuffer + offset, (total_pixels - offset) * sizeof(uint32_t));
        // Clear bottom 8 rows in RAM
        for (uint32_t i = total_pixels - offset; i < total_pixels; i++) {
            backbuffer[i] = BG_COLOR;
        }

        if (deferred_refresh) {
            screen_stale = true; // Set after the RAM copy is consistent again
        } else {
            // Send the whole picture to video memory. Scrolling inside video
            // memory itself would mean reading it back, which is far slower
            // than reading ordinary RAM.
            memcpy(fb_ptr, backbuffer, (size_t)total_pixels * sizeof(uint32_t));
        }
    } else {
        // Fallback before PMM is initialized
        for (uint32_t i = 0; i < total_pixels - offset; i++) {
            fb_ptr[i] = fb_ptr[i + offset];
        }
        for (uint32_t i = total_pixels - offset; i < total_pixels; i++) {
            fb_ptr[i] = BG_COLOR;
        }
    }
    
    cursor_y -= 8;
}

// Fill a rectangle of whole pixels with the background colour
static void clear_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (x1 > g_fb->width) x1 = g_fb->width;
    if (y1 > g_fb->height) y1 = g_fb->height;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) putpixel(x, y, BG_COLOR);
    }
}

// Colours and attributes: ESC [ n ; n ... m
static void apply_sgr(const ansi_action_t *action) {
    // "ESC [ m" with no number means reset, the same as 0
    int count = action->param_count ? action->param_count : 1;
    for (int i = 0; i < count; i++) {
        int n = action->param_count ? action->params[i] : 0;
        if (n == 0) { fg_index = DEFAULT_FG; bg_index = DEFAULT_BG; }
        else if (n == 1) { if (fg_index < 8) fg_index += 8; }           // Bold: shown as the bright colour
        else if (n == 22) { if (fg_index >= 8) fg_index -= 8; }         // Normal intensity
        else if (n >= 30 && n <= 37) fg_index = n - 30;
        else if (n == 39) fg_index = DEFAULT_FG;
        else if (n >= 40 && n <= 47) bg_index = n - 40;
        else if (n == 49) bg_index = DEFAULT_BG;
        else if (n >= 90 && n <= 97) fg_index = n - 90 + 8;
        else if (n >= 100 && n <= 107) bg_index = n - 100 + 8;
        // Anything else (underline, blink, ...) is not drawn: ignored
    }
    FG_COLOR = ansi_color(fg_index);
    BG_COLOR = ansi_color(bg_index);
}

static void draw_char(char c);

// One character of output, which may be text or part of an escape sequence
static void putchar_unlocked(char c) {
    if (!g_fb) return;

    ansi_action_t action = ansi_feed(&parser, c);
    uint32_t columns = g_fb->width / CELL, rows = g_fb->height / CELL;
    uint32_t col = cursor_x / CELL, row = cursor_y / CELL;
    // A count of 0 or none means 1 for the movement commands
    uint32_t n = (action.param_count && action.params[0] > 0) ? (uint32_t)action.params[0] : 1;

    switch (action.kind) {
        case ANSI_NOTHING:
            return;
        case ANSI_PRINT:
            draw_char(action.ch);
            return;
        case ANSI_SGR:
            apply_sgr(&action);
            return;
        case ANSI_CURSOR_TO: {
            uint32_t to_row = (action.param_count >= 1 && action.params[0] > 0) ? (uint32_t)action.params[0] : 1;
            uint32_t to_col = (action.param_count >= 2 && action.params[1] > 0) ? (uint32_t)action.params[1] : 1;
            if (to_row > rows) to_row = rows;
            if (to_col > columns) to_col = columns;
            cursor_y = (to_row - 1) * CELL;
            cursor_x = (to_col - 1) * CELL;
            return;
        }
        case ANSI_CURSOR_UP:    cursor_y = (row > n ? row - n : 0) * CELL; return;
        case ANSI_CURSOR_DOWN:  cursor_y = (row + n < rows ? row + n : rows - 1) * CELL; return;
        case ANSI_CURSOR_RIGHT: cursor_x = (col + n < columns ? col + n : columns - 1) * CELL; return;
        case ANSI_CURSOR_LEFT:  cursor_x = (col > n ? col - n : 0) * CELL; return;
        case ANSI_ERASE_LINE: {
            int mode = action.param_count ? action.params[0] : 0;
            uint32_t from = (mode == 0) ? cursor_x : 0;
            uint32_t to = (mode == 1) ? cursor_x + CELL : g_fb->width;
            clear_rect(from, cursor_y, to, cursor_y + CELL);
            return;
        }
        case ANSI_ERASE_DISPLAY: {
            int mode = action.param_count ? action.params[0] : 0;
            if (mode == 2) {
                clear_rect(0, 0, g_fb->width, g_fb->height);
            } else if (mode == 0) {         // From the cursor to the end of the screen
                clear_rect(cursor_x, cursor_y, g_fb->width, cursor_y + CELL);
                clear_rect(0, cursor_y + CELL, g_fb->width, g_fb->height);
            } else if (mode == 1) {         // From the start of the screen to the cursor
                clear_rect(0, 0, g_fb->width, cursor_y);
                clear_rect(0, cursor_y, cursor_x + CELL, cursor_y + CELL);
            }
            return;
        }
    }
}

static void draw_char(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y += 8;
        if (cursor_y + 8 > g_fb->height) scroll();
        return;
    }
    
    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    if (c == '\b') {
        if (cursor_x >= 8) {
            cursor_x -= 8;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    putpixel(cursor_x + x, cursor_y + y, BG_COLOR);
                }
            }
        }
        return;
    }
    
    unsigned char uc = (unsigned char)c;
    if (uc > 127) uc = '?'; // Basic font only goes up to 127
    
    char *glyph = font8x8_basic[uc];
    
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            if (glyph[y] & (1 << x)) {
                putpixel(cursor_x + x, cursor_y + y, FG_COLOR);
            } else {
                putpixel(cursor_x + x, cursor_y + y, BG_COLOR);
            }
        }
    }
    
    cursor_x += 8;
    if (cursor_x + 8 > g_fb->width) {
        cursor_x = 0;
        cursor_y += 8;
        if (cursor_y + 8 > g_fb->height) scroll();
    }
}

// The cursor and both copies of the screen are shared by every task that
// prints, and by interrupt handlers. Drawing one character is short, so it is
// simply done with interrupts off.
void console_putchar(char c) {
    if (!g_fb) return;
    uint64_t flags = irq_save();
    hide_cursor(); // It must not be scrolled, overwritten or left behind
    putchar_unlocked(c);
    irq_restore(flags);
}

void console_get_size(uint32_t *columns, uint32_t *rows) {
    if (columns) *columns = g_fb ? g_fb->width / CELL : 80;
    if (rows) *rows = g_fb ? g_fb->height / CELL : 25;
}

void console_get_cursor(uint32_t *column, uint32_t *row) {
    if (column) *column = cursor_x / CELL;
    if (row) *row = cursor_y / CELL;
}

uint32_t console_get_pixel(uint32_t x, uint32_t y) {
    if (!g_fb || !backbuffer || x >= g_fb->width || y >= g_fb->height) return 0;

    uint64_t flags = irq_save();
    hide_cursor(); // Report the text as drawn, without the cursor on top
    uint32_t color = backbuffer[y * (g_fb->pitch / 4) + x] & 0x00FFFFFF;
    irq_restore(flags);
    return color;
}

void console_write(const char *str) {
    while (*str) {
        console_putchar(*str++);
    }
}
