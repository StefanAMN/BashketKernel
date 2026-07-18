#ifndef CONSOLE_H
#define CONSOLE_H

#include "limine.h"
#include <stdint.h>

void console_init(struct limine_framebuffer *fb);
void console_putchar(char c);
void console_write(const char *str);

#endif
