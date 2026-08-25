/**
 * @file keyboard.c
 * @brief Implementation of PS/2 Keyboard Driver and Scancode Mapper
 */

#include "keyboard.h"
#include "io.h"
#include "interrupts.h"
#include "sched.h"

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

#define KEYBOARD_BUFFER_SIZE 256
static char kbd_buffer[KEYBOARD_BUFFER_SIZE];
static size_t kbd_head = 0;
static size_t kbd_tail = 0;
static bool shift_pressed = false;

// PS/2 Set 1 Scancode Mapping Table
static const char scancode_ascii[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char scancode_ascii_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

static void keyboard_irq_handler(struct registers *regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);

    // Track Shift key state
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = true;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = false;
        return;
    }

    // Ignore key release codes (bit 7 set)
    if (scancode & 0x80) return;

    char c = shift_pressed ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
    if (c != 0) {
        size_t next_head = (kbd_head + 1) % KEYBOARD_BUFFER_SIZE;
        if (next_head != kbd_tail) {
            kbd_buffer[kbd_head] = c;
            kbd_head = next_head;
        }
    }
}

void keyboard_init(void) {
    register_irq_handler(1, keyboard_irq_handler); // IRQ 1 = Interrupt 33
}

bool keyboard_has_char(void) {
    return kbd_head != kbd_tail;
}

char keyboard_getchar(void) {
    while (!keyboard_has_char()) {
        sched_yield();
    }

    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}
