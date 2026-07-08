#include "console.h"
#include "font.h"
#include <stddef.h>

static struct limine_framebuffer *g_fb = NULL;
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;

static const uint32_t FG_COLOR = 0xFFFFFFFF; // White
static const uint32_t BG_COLOR = 0x00000000; // Black

void console_init(struct limine_framebuffer *fb) {
    g_fb = fb;
    cursor_x = 0;
    cursor_y = 0;
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb->width || y >= g_fb->height) return;
    uint32_t *fb_ptr = g_fb->address;
    fb_ptr[y * (g_fb->pitch / 4) + x] = color;
}

static void scroll(void) {
    uint32_t *fb_ptr = g_fb->address;
    uint32_t pixels_per_row = g_fb->pitch / 4;
    uint32_t total_pixels = pixels_per_row * g_fb->height;
    uint32_t offset = pixels_per_row * 8; // 8 rows of pixels
    
    // Move everything up by 8 pixels
    for (uint32_t i = 0; i < total_pixels - offset; i++) {
        fb_ptr[i] = fb_ptr[i + offset];
    }
    
    // Clear the last 8 rows
    for (uint32_t i = total_pixels - offset; i < total_pixels; i++) {
        fb_ptr[i] = BG_COLOR;
    }
    
    cursor_y -= 8;
}

void console_putchar(char c) {
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

void console_write(const char *str) {
    while (*str) {
        console_putchar(*str++);
    }
}
