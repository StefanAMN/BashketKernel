/**
 * @file tty.h
 * @brief Virtual Terminal (TTY) Abstraction Interface
 */

#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize Virtual Terminal TTY subsystem.
 */
void tty_init(void);

/**
 * @brief Write data to TTY output stream.
 * @param str Input string pointer.
 * @param len Byte count to write.
 */
void tty_write(const char *str, size_t len);

/**
 * @brief Read data from TTY input queue.
 * @param buf Destination output buffer.
 * @param max_len Maximum bytes to read.
 * @return Number of bytes read.
 */
size_t tty_read(char *buf, size_t max_len);

#endif // TTY_H
