/**
 * @file tty.c
 * @brief The terminal: line discipline between the keyboard and read()
 */

#include "tty.h"
#include "keyboard.h"
#include "serial.h"
#include "console.h"
#include "spinlock.h"
#include "kprintf.h"
#include "kerrno.h"

#define CTRL_D 0x04
#define CTRL_U 0x15
#define DEL    0x7F

static char line[TTY_LINE_MAX];
static size_t line_len = 0;   // Characters collected so far
static size_t line_pos = 0;   // Next character to hand to a reader
static bool line_ready = false;

void tty_init(void) {
    line_len = 0;
    line_pos = 0;
    line_ready = false;
    kprintf("TTY Init: console line discipline ready.\n");
}

int64_t tty_write(const char *in, size_t count) {
    for (size_t i = 0; i < count; i++) {
        serial_putchar(in[i]);
        console_putchar(in[i]);
    }
    return (int64_t)count;
}

static void echo(const char *text, size_t count) {
    tty_write(text, count);
}

bool tty_input(char c) {
    uint64_t flags = irq_save();

    if (line_ready) {
        // The previous line is still waiting for a reader
    } else if (c == '\n' || c == '\r') {
        line[line_len++] = '\n'; // There is always room: see the limit below
        echo("\n", 1);
        line_ready = true;
    } else if (c == CTRL_D) {
        line_ready = true;       // Empty line: the reader sees end-of-file
    } else if (c == '\b' || c == DEL) {
        if (line_len > 0) {
            line_len--;
            echo("\b \b", 3);    // Step back, overwrite with a space, step back
        }
    } else if (c == CTRL_U) {
        while (line_len > 0) {
            line_len--;
            echo("\b \b", 3);
        }
    } else if (line_len < TTY_LINE_MAX - 1) { // Keep one place for the newline
        line[line_len++] = c;
        echo(&c, 1);
    }

    bool ready = line_ready;
    irq_restore(flags);
    return ready;
}

int64_t tty_read(char *out, size_t count) {
    if (count == 0) return 0;

    // Interrupts stay off from here on, so that taking a character from the
    // keyboard and running it through tty_input is one step: two programs
    // reading the terminal can not interleave inside it. keyboard_getchar
    // sleeps (and lets everything else run) while there is nothing typed.
    uint64_t flags = irq_save();
    while (!line_ready) {
        char c = keyboard_getchar();
        if (c == 0) {
            // Interrupted by a signal. What has been typed so far stays in
            // the line for the next read.
            irq_restore(flags);
            return -EINTR;
        }
        tty_input(c);
    }

    size_t n = 0;
    while (n < count && line_pos < line_len) {
        out[n++] = line[line_pos++];
    }

    if (line_pos == line_len) { // Line used up: start collecting the next one
        line_len = 0;
        line_pos = 0;
        line_ready = false;
    }

    irq_restore(flags);
    return (int64_t)n;
}
