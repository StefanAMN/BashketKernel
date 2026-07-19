/**
 * @file serial.c
 * @brief Implementation of 16550 UART Serial Communication Driver
 */

#include "serial.h"
#include "io.h"
#include "interrupts.h"
#include <stddef.h>
#include <stdbool.h>

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

#define SERIAL_BUFFER_SIZE 256
static char serial_buffer[SERIAL_BUFFER_SIZE];
static volatile size_t serial_head = 0;
static volatile size_t serial_tail = 0;

static void serial_irq_handler(struct registers *regs) {
    (void)regs;
    while (inb(COM1 + 5) & 0x01) {
        char c = (char)inb(COM1);
        if (c == '\r') c = '\n';
        if (c == 127) c = '\b';
        if (c != 0) {
            size_t next_head = (serial_head + 1) % SERIAL_BUFFER_SIZE;
            if (next_head != serial_tail) {
                serial_buffer[serial_head] = c;
                serial_head = next_head;
            }
        }
    }
}

void serial_init(void) {
    outb(COM1 + 1, 0x00); // Disable all interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB (Divisor Latch Access Bit) to set baud rate
    outb(COM1 + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    outb(COM1 + 1, 0x00); //                (hi byte)
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit (8N1), clear DLAB
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
    outb(COM1 + 1, 0x01); // Enable Received Data Available Interrupt (IER bit 0)

    // Register IRQ 4 (Interrupt 36)
    register_irq_handler(4, serial_irq_handler);

    // Unmask IRQ 4 on 8259 Master PIC
    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~(1 << 4));
}

static int is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putchar(char c) {
    while (is_transmit_empty() == 0);
    outb(COM1, c);
}

void serial_write(const char *str) {
    if (!str) return;
    for (size_t i = 0; str[i] != '\0'; i++) {
        serial_putchar(str[i]);
    }
}

bool serial_has_char(void) {
    if (serial_head != serial_tail) {
        return true;
    }
    // Bit 0 of Line Status Register (COM1 + 5) is Data Ready
    return (inb(COM1 + 5) & 0x01) != 0;
}

char serial_getchar(void) {
    if (serial_head != serial_tail) {
        char c = serial_buffer[serial_tail];
        serial_tail = (serial_tail + 1) % SERIAL_BUFFER_SIZE;
        return c;
    }

    while (!serial_has_char()) {
        asm volatile("pause");
    }

    if (serial_head != serial_tail) {
        char c = serial_buffer[serial_tail];
        serial_tail = (serial_tail + 1) % SERIAL_BUFFER_SIZE;
        return c;
    }

    char c = (char)inb(COM1);
    if (c == '\r') c = '\n';
    if (c == 127) c = '\b';
    return c;
}
