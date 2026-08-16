/**
 * @file tty.c
 * @brief Implementation of Virtual Terminal (TTY) Subsystem
 */

#include "tty.h"
#include "kprintf.h"
#include "string_util.h"

#define TTY_BUF_SIZE 512

static char tty_buffer[TTY_BUF_SIZE];
static size_t tty_head = 0;
static size_t tty_tail = 0;
static size_t tty_count = 0;

void tty_init(void) {
    memset(tty_buffer, 0, sizeof(tty_buffer));
    tty_head = tty_tail = tty_count = 0;
    kprintf("TTY Init: Virtual terminal subsystem initialized.\n");
}

void tty_write(const char *str, size_t len) {
    if (!str || len == 0) return;
    for (size_t i = 0; i < len; i++) {
        if (tty_count < TTY_BUF_SIZE) {
            tty_buffer[tty_head] = str[i];
            tty_head = (tty_head + 1) % TTY_BUF_SIZE;
            tty_count++;
        }
    }
}

size_t tty_read(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    size_t read_bytes = 0;
    while (read_bytes < max_len && tty_count > 0) {
        buf[read_bytes] = tty_buffer[tty_tail];
        tty_tail = (tty_tail + 1) % TTY_BUF_SIZE;
        tty_count--;
        read_bytes++;
    }
    return read_bytes;
}
