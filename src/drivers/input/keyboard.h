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

// The PS/2 controller's second ("auxiliary") port, for the mouse driver.
// It shares the controller's data port with the keyboard, so access to it
// goes through here.

/**
 * @brief Enable the second port and its interrupt (IRQ 12).
 */
bool ps2_aux_enable(void);

/**
 * @brief Send one command byte to the device on the second port.
 * @return true if it acknowledged.
 */
bool ps2_aux_command(uint8_t command);

/**
 * @brief Fetch a byte the device on the second port has sent, if one is
 *        waiting.
 */
bool ps2_read_aux(uint8_t *byte);

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
// Returns 0 if the calling process was interrupted by a signal before any
// character arrived (kernel threads are never interrupted).
char keyboard_getchar(void);

#endif // KEYBOARD_H
