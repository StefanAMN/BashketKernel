/**
 * @file keyboard.h
 * @brief PS/2 Keyboard Controller Driver (IRQ 1 / Port 0x60) Interface
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize PS/2 Keyboard driver and register IRQ 1 handler.
 */
void keyboard_init(void);

/**
 * @brief Check if input character is available in keyboard buffer.
 * @return true if character is buffered, false otherwise.
 */
bool keyboard_has_char(void);

/**
 * @brief Wake tasks sleeping in keyboard_getchar. Called by the keyboard and
 *        serial interrupt handlers after they have buffered input.
 */
void keyboard_input_arrived(void);

/**
 * @brief Retrieve next character from keyboard buffer (blocking if empty).
 * @return ASCII character byte.
 */
char keyboard_getchar(void);

#endif // KEYBOARD_H
