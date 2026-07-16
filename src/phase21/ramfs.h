/**
 * @file ramfs.h
 * @brief Dynamic RAM Filesystem Subsystem Interface
 */

#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize RamFS and mount /tmp directory.
 */
void ramfs_init(void);

#endif // RAMFS_H
