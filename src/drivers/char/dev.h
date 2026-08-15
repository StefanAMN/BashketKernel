/**
 * @file dev.h
 * @brief Device nodes under /dev
 */

#ifndef DEV_H
#define DEV_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Make /dev (a mount point) with null, zero, random and console for
 *        everyone, and, where the hardware is there, vda (the first disk,
 *        root only), fb0 (the screen) and mouse. Called once at boot, after
 *        the drivers.
 */
void dev_init(void);

#endif // DEV_H
