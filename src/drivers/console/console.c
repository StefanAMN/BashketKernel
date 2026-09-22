#include "console.h"
#include "font.h"
#include "pmm.h"
#include "kstring.h"
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

static const uint32_t FG_COLOR = 0xFFFFFFFF; // White
static const uint32_t BG_COLOR = 0x00000000; // Black

void console_init(struct limine_framebuffer *fb) {
    g_fb = fb;
    cursor_x = 0;
    cursor_y = 0;
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

static void putchar_unlocked(char c) {
    if (!g_fb) return;
    
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
    putchar_unlocked(c);
    irq_restore(flags);
}

void console_write(const char *str) {
    while (*str) {
        console_putchar(*str++);
    }
}
