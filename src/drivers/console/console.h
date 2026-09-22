#ifndef CONSOLE_H
#define CONSOLE_H

#include "limine.h"
#include <stdint.h>
#include <stdbool.h>

void console_init(struct limine_framebuffer *fb);
void console_enable_backbuffer(void);
void console_putchar(char c);

// Let scrolling update only the in-RAM copy of the screen and leave it to
// console_refresh to show the result. Turn on once something (the timer)
// calls console_refresh regularly.
void console_set_deferred_refresh(bool enabled);

// Send the picture to video memory if scrolling left the screen out of date.
// Cheap when nothing changed. Also call it before halting, so the last
// messages are visible.
void console_refresh(void);
void console_write(const char *str);

#endif
