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

// Raw mode: a full-screen program (an editor) wants every key as it is typed,
// with no echo and no line editing. The mode belongs to the process that
// asked for it and ends when that process does, however it ends, so that a
// crashed editor does not leave the shell with an unusable terminal.
static bool raw_mode = false;
static uint32_t raw_owner = 0;

// Arrow keys arrive as ESC [ A and the like. In line mode they are not text:
// this tracks an escape sequence in the input so that it can be dropped.
static int input_escape = 0; // 0: text, 1: seen ESC, 2: inside ESC [ ...

void tty_init(void) {
    line_len = 0;
    line_pos = 0;
    line_ready = false;
    raw_mode = false;
    input_escape = 0;
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
    } else if (input_escape == 1) {
        input_escape = (c == '[') ? 2 : 0;          // ESC [ starts a sequence; ESC + anything else is dropped
    } else if (input_escape == 2) {
        if (c >= 0x40 && c <= 0x7E) input_escape = 0; // The final character of the sequence
    } else if (c == 0x1B) {
        input_escape = 1;
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
    } else if ((unsigned char)c < 0x20 && c != '\t') {
        // Other control characters have no meaning in a line: ignored
    } else if (line_len < TTY_LINE_MAX - 1) { // Keep one place for the newline
        line[line_len++] = c;
        echo(&c, 1);
    }

    bool ready = line_ready;
    irq_restore(flags);
    return ready;
}

void tty_set_raw(bool raw, uint32_t owner_pid) {
    uint64_t flags = irq_save();
    raw_mode = raw;
    raw_owner = raw ? owner_pid : 0;
    if (raw) {
        // Whatever was half typed in line mode is not wanted any more
        line_len = 0;
        line_pos = 0;
        line_ready = false;
        input_escape = 0;
    }
    irq_restore(flags);
}

bool tty_is_raw(void) {
    return raw_mode;
}

void tty_process_exited(uint32_t pid) {
    if (raw_mode && raw_owner == pid) tty_set_raw(false, 0);
}

// Raw mode: wait for one key, then return whatever else has already arrived
static int64_t read_raw(char *out, size_t count) {
    char first = keyboard_getchar();
    if (first == 0) return -EINTR;

    size_t n = 0;
    out[n++] = first;
    while (n < count && keyboard_has_char()) {
        char c = keyboard_getchar();
        if (c == 0) break;
        out[n++] = c;
    }
    return (int64_t)n;
}

int64_t tty_read(char *out, size_t count) {
    if (count == 0) return 0;
    if (raw_mode) return read_raw(out, count);

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
