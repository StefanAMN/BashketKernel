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
#include "random.h"
#include "signal.h"
#include "tty.h"
#include "process.h"
#include "kprintf.h"


#define KEYBOARD_BUFFER_SIZE 256
static char kbd_buffer[KEYBOARD_BUFFER_SIZE];
static size_t kbd_head = 0;
static size_t kbd_tail = 0;
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool extended = false;   // The previous scancode was the 0xE0 prefix

#define CTRL_C 0x03
#define CTRL_Z 0x1A

// Store one input character. Ctrl-C is not data: it interrupts the running
// programs instead. Ctrl-Z stops the foreground job, if a shell has set one
// up and the terminal is not in raw mode (a full-screen program asked for
// every key). Caller must hold kbd_lock.
static void kbd_buffer_push(char c) {
    if (c == CTRL_C) {
        process_interrupt_all();
        return;
    }
    if (c == CTRL_Z && !tty_is_raw() && process_signal_foreground(SIGTSTP)) return;

    size_t next_head = (kbd_head + 1) % KEYBOARD_BUFFER_SIZE;
    if (next_head != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next_head;
    }
}

// Guards the ring buffer, the shift state and the PS/2 data port, which the
// IRQ handler and the polling path in keyboard_getchar both use.
static spinlock_t kbd_lock = SPINLOCK_RANKED("kbd", LOCK_RANK_INPUT);

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

// --- The controller's second port: the mouse ------------------------------------
//
// Keyboard and mouse bytes arrive at the same data port, 0x60, and the
// status port says whose the waiting byte is (bit 5: the auxiliary device,
// Intel 8042 data sheet). Whoever reads the port must hold kbd_lock, so that
// "it is mine" and "I have read it" can not be separated by the other.

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_AUX         0x20
#define PS2_ACK                0xFA
#define PS2_SPINS              200000   // How long to wait for the controller before giving up

bool ps2_read_aux(uint8_t *byte) {
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    uint8_t status = inb(0x64);
    bool mine = (status & PS2_STATUS_OUTPUT_FULL) && (status & PS2_STATUS_AUX);
    if (mine) *byte = inb(0x60);
    spinlock_release_irqrestore(&kbd_lock, flags);
    return mine;
}

// Wait until the controller will take a byte. Caller holds kbd_lock.
static bool ps2_can_write(void) {
    for (int i = 0; i < PS2_SPINS; i++) {
        if (!(inb(0x64) & PS2_STATUS_INPUT_FULL)) return true;
        asm volatile("pause");
    }
    return false;
}

// Wait for a byte from the auxiliary device. Keyboard bytes that arrive
// meanwhile go to the keyboard. Caller holds kbd_lock.
static bool ps2_wait_aux_byte(uint8_t *byte) {
    for (int i = 0; i < PS2_SPINS; i++) {
        uint8_t status = inb(0x64);
        if (status & PS2_STATUS_OUTPUT_FULL) {
            if (status & PS2_STATUS_AUX) {
                *byte = inb(0x60);
                return true;
            }
            keyboard_handle_port();
        }
        asm volatile("pause");
    }
    return false;
}

bool ps2_aux_enable(void) {
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    bool ok = false;

    // Controller command 0xA8: enable the second port. Then, in the
    // controller's configuration byte (read with 0x20, written with 0x60):
    // bit 1 makes it raise IRQ 12 for the second port, bit 5 would stop
    // that port's clock.
    if (ps2_can_write()) {
        outb(0x64, 0xA8);
        if (ps2_can_write()) {
            outb(0x64, 0x20);
            uint8_t config = 0;
            bool got = false;
            for (int i = 0; i < PS2_SPINS && !got; i++) {
                uint8_t status = inb(0x64);
                // The answer to a controller command comes back without the AUX bit
                if ((status & PS2_STATUS_OUTPUT_FULL) && !(status & PS2_STATUS_AUX)) {
                    config = inb(0x60);
                    got = true;
                }
            }
            if (got && ps2_can_write()) {
                config = (uint8_t)((config | 0x02) & ~0x20);
                outb(0x64, 0x60);
                if (ps2_can_write()) {
                    outb(0x60, config);
                    ok = true;
                }
            }
        }
    }
    spinlock_release_irqrestore(&kbd_lock, flags);
    return ok;
}

bool ps2_aux_command(uint8_t command) {
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    bool acked = false;

    // Controller command 0xD4: "the next byte written to the data port is
    // for the second port"
    if (ps2_can_write()) {
        outb(0x64, 0xD4);
        if (ps2_can_write()) {
            outb(0x60, command);
            // The device answers ACK. (Anything else it had already sent,
            // such as a movement, may come first.)
            uint8_t byte;
            for (int tries = 0; tries < 8 && !acked && ps2_wait_aux_byte(&byte); tries++) acked = (byte == PS2_ACK);
        }
    }
    spinlock_release_irqrestore(&kbd_lock, flags);
    return acked;
}

// IRQ 1: decode the scancode, then wake whoever waits for input
static void keyboard_irq_handler(struct registers *regs) {
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    keyboard_handle_port();
    spinlock_release_irqrestore(&kbd_lock, flags);
    random_add_event(regs ? regs->rip : 0); // When a key is pressed, and what was running, is nobody's to choose

    keyboard_input_arrived();
}

// Readers sleep on this address; both input interrupts wake it. Input can
// arrive in either of two buffers, each with its own lock, so "both are
// empty" is not something one of those locks can keep true. input_lock does:
// the reader holds it from checking the buffers until it is asleep, and an
// interrupt handler takes it to announce what it has just stored.
static char input_channel;
static spinlock_t input_lock = SPINLOCK_RANKED("input", LOCK_RANK_INPUT_WAIT);

void keyboard_input_arrived(void) {
    uint64_t flags = spinlock_acquire_irqsave(&input_lock);
    sched_wakeup(&input_channel);
    spinlock_release_irqrestore(&input_lock, flags);
}

// Wait (a bounded time) for the answer to a controller command. Caller
// holds kbd_lock.
static bool ps2_can_read(void) {
    for (int i = 0; i < PS2_SPINS; i++) {
        if (inb(0x64) & PS2_STATUS_OUTPUT_FULL) return true;
        asm volatile("pause");
    }
    return false;
}

void keyboard_init(void) {
    register_irq_handler(1, keyboard_irq_handler); // IRQ 1 = Interrupt 33

    // Every wait is bounded: a machine may have no PS/2 controller at all
    // (port 0x64 then reads 0xFF, "busy" for ever), and that must not stop
    // the boot. Its keyboard is then the serial line.
    uint64_t flags = spinlock_acquire_irqsave(&kbd_lock);
    bool ok = false;

    // Enable the first port (command 0xAE), then read the configuration
    // byte (0x20), set bit 0 (raise IRQ 1) and clear bit 4 (stop that
    // port's clock), and write it back (0x60)
    if (ps2_can_write()) {
        outb(0x64, 0xAE);
        if (ps2_can_write()) {
            outb(0x64, 0x20);
            if (ps2_can_read()) {
                uint8_t config = inb(0x60);
                config = (uint8_t)((config | 0x01) & ~0x10);
                if (ps2_can_write()) {
                    outb(0x64, 0x60);
                    if (ps2_can_write()) {
                        outb(0x60, config);
                        ok = true;
                    }
                }
            }
        }
    }

    // Throw away anything stale the controller holds (at most its buffer's worth)
    for (int i = 0; ok && i < 16 && (inb(0x64) & PS2_STATUS_OUTPUT_FULL); i++) (void)inb(0x60);
    spinlock_release_irqrestore(&kbd_lock, flags);

    if (!ok) {
        kprintf("keyboard: no PS/2 controller answered; input comes from the serial line only\n");
        return;
    }
    // Until the I/O APIC takes over (ioapic_init masks the PIC again), key
    // presses arrive through the 8259 PIC: unmask IRQ 1 there
    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~(1 << 1));
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
    // input_lock is held between finding both buffers empty and going to
    // sleep, so a key that arrives in between still wakes us.
    uint64_t flags = spinlock_acquire_irqsave(&input_lock);
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
        sched_sleep(&input_channel, &input_lock, 0);
    }

    spinlock_release_irqrestore(&input_lock, flags);
    return c;
}
