/**
 * @file keyboard.c
 * @brief Implementation of PS/2 Keyboard Driver & Serial Input Multiplexer
 */

#include "keyboard.h"
#include "io.h"
#include "interrupts.h"
#include "sched.h"
#include "serial.h"
#include "spinlock.h"
#include "process.h"

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

#define KEYBOARD_BUFFER_SIZE 256
static char kbd_buffer[KEYBOARD_BUFFER_SIZE];
static size_t kbd_head = 0;
static size_t kbd_tail = 0;
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool extended = false;   // The previous scancode was the 0xE0 prefix

#define CTRL_C 0x03

// Store one input character. Ctrl-C is not data: it interrupts the running
// programs instead. Caller must hold kbd_lock.
static void kbd_buffer_push(char c) {
    if (c == CTRL_C) {
        process_interrupt_all();
        return;
    }

    size_t next_head = (kbd_head + 1) % KEYBOARD_BUFFER_SIZE;
    if (next_head != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next_head;
    }
}

// Guards the ring buffer, the shift state and the PS/2 data port, which the
// IRQ handler and the polling path in keyboard_getchar both use.
static spinlock_t kbd_lock = SPINLOCK_INIT;

// PS/2 Set 1 Scancode Mapping Table (Normal)
static const char scancode_ascii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', /* 0x00 - 0x0E */
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',      /* 0x0F - 0x1C */
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',           /* 0x1D - 0x29 */
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,             /* 0x2A - 0x36 */
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,             /* 0x37 - 0x43 (F1-F9) */
    0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', /* 0x44 - 0x52 (Numpad) */
    '.'                                                                           /* 0x53 */
};

// PS/2 Set 1 Scancode Mapping Table (Shift)
static const char scancode_ascii_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', /* 0x00 - 0x0E */
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',      /* 0x0F - 0x1C */
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',           /* 0x1D - 0x29 */
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,             /* 0x2A - 0x36 */
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,             /* 0x37 - 0x43 */
    0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', /* 0x44 - 0x52 */
    '.'                                                                           /* 0x53 */
};

static void ps2_wait_write(void) {
    while (inb(0x64) & 2) {
        asm volatile("pause");
    }
}

static void ps2_wait_read(void) {
    while (!(inb(0x64) & 1)) {
        asm volatile("pause");
    }
}

// Caller must hold kbd_lock.
static void keyboard_handle_port(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return; // No data in output buffer
    if (status & 0x20) return;    // Data is from AUX mouse, not keyboard

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

    // 0xE0 announces an "extended" key: the arrows, Home, End, Delete and the
    // right-hand Ctrl. They are passed on the way a serial terminal sends
    // them, as ESC [ and a letter, so programs handle both the same way.
    if (scancode == 0xE0) {
        extended = true;
        return;
    }
    if (extended) {
        extended = false;
        const char *sequence = NULL;
        switch (scancode) {
            case 0x48: sequence = "\033[A"; break;  // Up
            case 0x50: sequence = "\033[B"; break;  // Down
            case 0x4D: sequence = "\033[C"; break;  // Right
            case 0x4B: sequence = "\033[D"; break;  // Left
            case 0x47: sequence = "\033[H"; break;  // Home
            case 0x4F: sequence = "\033[F"; break;  // End
            case 0x53: sequence = "\033[3~"; break; // Delete
            case 0x1D: ctrl_pressed = true; break;  // Right Ctrl down
            case 0x9D: ctrl_pressed = false; break; // Right Ctrl up
            default: break;                         // Other extended keys and all releases
        }
        for (; sequence && *sequence; sequence++) kbd_buffer_push(*sequence);
        return;
    }

    // Track the (left) Ctrl key
    if (scancode == 0x1D) {
        ctrl_pressed = true;
        return;
    }
    if (scancode == 0x9D) {
        ctrl_pressed = false;
        return;
    }

    // Ignore key release codes (bit 7 set)
    if (scancode & 0x80) return;

    char c = shift_pressed ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
    if (c == 0) return;

    // Ctrl with a letter gives the control character: Ctrl-C is 3, Ctrl-D is 4
    if (ctrl_pressed && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c = (char)(c & 0x1F);
    }
    kbd_buffer_push(c);
}

static void keyboard_irq_handler(struct registers *regs) {
    (void)regs;
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    keyboard_handle_port();
    spinlock_release_irqrestore(&kbd_lock, flags);

    keyboard_input_arrived();
}

// Readers sleep on this address; both input interrupts wake it
static char input_channel;

void keyboard_input_arrived(void) {
    sched_wakeup(&input_channel);
}

void keyboard_init(void) {
    register_irq_handler(1, keyboard_irq_handler); // IRQ 1 = Interrupt 33

    // 1. Enable first PS/2 port (Keyboard)
    ps2_wait_write();
    outb(0x64, 0xAE);

    // 2. Read PS/2 Controller Command Byte (0x20)
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    uint8_t config = inb(0x60);

    // 3. Enable IRQ 1 (bit 0 = 1) and enable first PS/2 port (bit 4 = 0)
    config |= 0x01;
    config &= ~(0x10);

    // 4. Write updated Controller Command Byte (0x60)
    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, config);

    // 5. Explicitly unmask IRQ 1 (Keyboard) on 8259 Master PIC
    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~(1 << 1));

    // 6. Clear any pending/stale scancode in PS/2 controller data buffer
    while (inb(0x64) & 1) {
        (void)inb(0x60);
    }
}

// Take one character out of the ring buffer. Returns 0 if it is empty.
static char kbd_buffer_pop(void) {
    char c = 0;
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    // Pick up a scancode the controller is holding but has not delivered an
    // interrupt for (polling fallback), using the same decoder as the IRQ.
    keyboard_handle_port();
    if (kbd_head != kbd_tail) {
        c = kbd_buffer[kbd_tail];
        kbd_tail = (kbd_tail + 1) % KEYBOARD_BUFFER_SIZE;
    }
    spinlock_release_irqrestore(&kbd_lock, flags);
    return c;
}

bool keyboard_has_char(void) {
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    keyboard_handle_port();
    bool has = (kbd_head != kbd_tail);
    spinlock_release_irqrestore(&kbd_lock, flags);

    return has || serial_has_char();
}

char keyboard_getchar(void) {
    // Interrupts off between finding both buffers empty and going to sleep,
    // so a key that arrives in between still wakes us.
    uint64_t flags = irq_save();
    char c;

    while (1) {
        c = kbd_buffer_pop();
        if (c != 0) break;

        if (serial_has_char()) {
            c = serial_getchar();
            if (c != 0) break;
        }

        if (sched_interrupted()) {
            c = 0; // No character: a signal handler has to run first
            break;
        }
        sched_block(&input_channel, 0);
    }

    irq_restore(flags);
    return c;
}
