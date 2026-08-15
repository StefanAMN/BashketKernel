/**
 * @file keyboard.h
 * @brief PS/2 Keyboard Controller Driver (IRQ 1 / Port 0x60) Interface
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Register the IRQ 1 handler and switch the PS/2 controller's first
 *        port on. Every wait is bounded: without a controller this reports
 *        it and returns, and input comes from the serial line only.
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
 * @brief Whether a character is waiting, from the keyboard or the serial
 *        line (both are polled too, for input whose interrupt is late).
 */
bool keyboard_has_char(void);

/**
 * @brief Wake tasks sleeping in keyboard_getchar. Called by the keyboard and
 *        serial interrupt handlers after they have buffered input.
 */
void keyboard_input_arrived(void);

/**
 * @brief The next character typed, from the keyboard or the serial line,
 *        sleeping until there is one. Arrow and editing keys arrive as ANSI
 *        sequences (ESC [ A ...), Ctrl+letter as its control character.
 * @return The character, or 0 if the calling process was interrupted by a
 *         signal first (kernel threads are never interrupted).
 */
char keyboard_getchar(void);

#endif // KEYBOARD_H
