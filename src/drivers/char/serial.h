/**
 * @file serial.h
 * @brief 16550 UART Serial Communication Driver Interface
 * 
 * Provides serial logging over COM1 (port 0x3F8). Serial ports are crucial in 
 * early OS kernel development because they operate reliably over QEMU text 
 * redirection (`-nographic` / `-serial stdio`) even if the graphical framebuffer crashes.
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#define COM1 0x3F8 // Base I/O port address for COM1 serial port

/**
 * @brief Initialize the COM1 16550 UART serial controller.
 * Configures baud rate to 38400, 8 data bits, no parity, 1 stop bit (8N1), and enables FIFO.
 */
void serial_init(void);

/**
 * @brief Output a single ASCII character to COM1 serial.
 * @param c The character to send.
 */
void serial_putchar(char c);

/**
 * @brief Output a null-terminated string to COM1 serial.
 * @param str The string buffer to send.
 */
void serial_write(const char *str);

#endif // SERIAL_H
