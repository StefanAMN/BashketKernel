/**
 * @file serial.h
 * @brief 16550 UART Serial Communication Driver Interface
 * 
 * Provides serial logging and input reading over COM1 (port 0x3F8).
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#define COM1 0x3F8 // Base I/O port address for COM1 serial port

/**
 * @brief Initialize the COM1 16550 UART serial controller.
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

/**
 * @brief Check if input character is available on COM1 serial port.
 * @return true if character is ready to be read, false otherwise.
 */
bool serial_has_char(void);

/**
 * @brief Read a character from COM1 serial port (blocking if empty).
 * @return ASCII character byte.
 */
char serial_getchar(void);

#endif // SERIAL_H
