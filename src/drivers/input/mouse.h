/**
 * @file mouse.h
 * @brief PS/2 mouse, read by programs from /dev/mouse
 *
 * The mouse hangs off the same controller as the keyboard (its second, or
 * "auxiliary", port) and has its own interrupt, IRQ 12. It reports movement
 * in packets of three bytes (the standard PS/2 mouse protocol):
 *
 *     byte 0   bit 0-2  left, right, middle button down
 *              bit 3    always 1 (the only way to tell where a packet starts)
 *              bit 4,5  sign of the X and of the Y movement
 *              bit 6,7  X, Y overflow: the movement did not fit
 *     byte 1   X movement, low 8 bits (positive: right)
 *     byte 2   Y movement, low 8 bits (positive: up, the opposite of screen rows)
 *
 * The kernel does not keep a pointer position: where the pointer is, and
 * whether there is one, is the business of the program reading the device.
 * It hands movements on in nearly the same form, three bytes per event:
 *
 *     byte 0   buttons (bits 0-2), bit 3 set
 *     byte 1   X movement, signed, -127..127
 *     byte 2   Y movement, signed, -127..127 (positive: up)
 *
 * A read blocks until there is at least one event and returns as many whole
 * events as fit.
 */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MOUSE_EVENT_SIZE 3
#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

/**
 * @brief Switch the mouse on, if the controller has one.
 * @return 0, or -1 if nothing answered on the auxiliary port.
 */
int mouse_init(void);

bool mouse_present(void);

/**
 * @brief Feed the driver one byte from the mouse. Called by the interrupt
 *        handler; separate so that tests can be the mouse.
 */
void mouse_handle_byte(uint8_t byte);

/**
 * @brief Take events out of the queue, as /dev/mouse returns them.
 * @param block Sleep while there are none (otherwise return 0).
 * @return Bytes stored (a multiple of MOUSE_EVENT_SIZE), or -EINTR.
 */
int64_t mouse_read(void *buf, size_t count, bool block);

/**
 * @brief Forget queued events and any packet half received.
 */
void mouse_flush(void);

#endif // MOUSE_H
