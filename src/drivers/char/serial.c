/**
 * @file serial.c
 * @brief Implementation of 16550 UART Serial Communication Driver
 */

#include "serial.h"
#include "io.h"
#include <stddef.h>

void serial_init(void) {
    outb(COM1 + 1, 0x00); // Disable all interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB (Divisor Latch Access Bit) to set baud rate
    outb(COM1 + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(COM1 + 1, 0x00); //                (hi byte)
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit (8N1), clear DLAB
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
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
    // Bit 0 of Line Status Register (COM1 + 5) is Data Ready
    return (inb(COM1 + 5) & 0x01) != 0;
}

char serial_getchar(void) {
    while (!serial_has_char()) {
        asm volatile("pause");
    }
    return (char)inb(COM1);
}
