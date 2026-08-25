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
    // Bit 5 of Line Status Register (COM1 + 5) is Transmit Holding Register Empty
    return inb(COM1 + 5) & 0x20;
}

void serial_putchar(char c) {
    // Wait until transmit buffer is empty before sending next byte
    while (is_transmit_empty() == 0);
    outb(COM1, c);
}

void serial_write(const char *str) {
    if (!str) return;
    for (size_t i = 0; str[i] != '\0'; i++) {
        serial_putchar(str[i]);
    }
}
