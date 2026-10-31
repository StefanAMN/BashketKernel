/**
 * @file procfs.h
 * @brief Pseudo-Filesystem (/proc) Subsystem Interface
 */

#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>

/**
 * @brief Create /proc (a mount point, hiding any /proc directory on the
 *        root disk) with its files: version, uptime, timers, meminfo, tasks,
 *        cpuinfo, kmsg and mounts. Each is generated when an open file
 *        first reads it (or reads from position 0 again), and kept with that
 *        file; none can be written. Call once at boot, after the root is
 *        mounted.
 */
void procfs_init(void);

#endif // PROCFS_H
