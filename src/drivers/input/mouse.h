/**
 * @file mouse.h
 * @brief PS/2 Auxiliary Mouse Driver Interface (IRQ 12 / Vector 44)
 */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t x;
    int32_t y;
    bool left_button;
    bool right_button;
    bool middle_button;
} mouse_state_t;

/**
 * @brief Initialize PS/2 Mouse device and register IRQ 12 interrupt handler.
 */
void mouse_init(void);

/**
 * @brief Retrieve current mouse position and button state.
 * @param out_state Output structure pointer.
 */
void mouse_get_state(mouse_state_t *out_state);

#endif // MOUSE_H
