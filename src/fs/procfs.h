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
 *        cpuinfo, kmsg and mounts. Each is generated afresh at every read
 *        and cannot be written. Call once at boot, after the root is mounted.
 */
void procfs_init(void);

#endif // PROCFS_H
