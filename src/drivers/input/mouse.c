/**
 * @file mouse.c
 * @brief Implementation of PS/2 Auxiliary Mouse Driver
 */

#include "mouse.h"
#include "kprintf.h"
#include "idt.h"
#include "interrupts.h"
#include "io.h"

static mouse_state_t current_mouse_state = { .x = 640, .y = 400, .left_button = false, .right_button = false, .middle_button = false };

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) { // Data ready
        while (timeout--) {
            if ((inb(0x64) & 1) == 1) return;
        }
    } else { // Signal ready
        while (timeout--) {
            if ((inb(0x64) & 2) == 0) return;
        }
    }
}

static void mouse_write(uint8_t val) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, val);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

static void mouse_irq_handler(struct registers *regs) {
    (void)regs;
    while (inb(0x64) & 1) {
        (void)inb(0x60);
    }
}

void mouse_init(void) {
    register_irq_handler(12, mouse_irq_handler);

    // Enable PS/2 auxiliary mouse controller port
    mouse_wait(1);
    outb(0x64, 0xA8);

    // Enable IRQ 12 interrupts in controller byte
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    uint8_t status = inb(0x60) | 2;
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    // Set default mouse settings & enable packet streaming
    mouse_write(0xF6); // Set defaults
    mouse_read();
    mouse_write(0xF4); // Enable packet reporting
    mouse_read();

    kprintf("PS/2 Mouse Init: Auxiliary mouse driver initialized (IRQ 12)\n");
}

void mouse_get_state(mouse_state_t *out_state) {
    if (!out_state) return;
    *out_state = current_mouse_state;
}
