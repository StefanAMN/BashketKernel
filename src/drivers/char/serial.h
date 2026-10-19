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
 * @brief Set COM1 up (115200 baud, 8N1, FIFOs on), interrupt on every
 *        received byte (IRQ 4), and register the handler. Called first
 *        thing at boot: kprintf writes here from then on.
 */
void serial_init(void);

/**
 * @brief Send one byte, spinning until the UART has room for it. Safe from
 *        any context; the caller serialises (kprintf's lock).
 */
void serial_putchar(char c);

/**
 * @brief Whether a received byte is waiting (drains the UART into the
 *        buffer first, for bytes whose interrupt has not come yet).
 */
bool serial_has_char(void);

/**
 * @brief Take the next received byte, spinning (not sleeping) until there
 *        is one. Carriage return arrives as '\n' and DEL as '\b'; Ctrl-C
 *        and Ctrl-Z never arrive: they signal the foreground programs.
 */
char serial_getchar(void);

#endif // SERIAL_H
