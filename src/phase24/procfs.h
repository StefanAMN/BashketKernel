/**
 * @file procfs.h
 * @brief Pseudo-Filesystem (/proc) Subsystem Interface
 */

#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>

/**
 * @brief Initialize ProcFS pseudo-filesystem and mount virtual nodes in '/proc'.
 */
void procfs_init(void);

#endif // PROCFS_H
