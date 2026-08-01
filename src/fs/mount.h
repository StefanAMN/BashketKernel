/**
 * @file mount.h
 * @brief Mount table: which filesystem provides which part of the tree
 *
 * The directory tree is assembled from several sources: files kept in
 * memory, the initrd, generated files (/proc), devices (/dev) and the FAT32
 * volume on the disk. Each registers here where it is attached. The table is
 * a record for `mount` and /proc/mounts; path lookup does not consult it,
 * because every source attaches its nodes directly into the one tree.
 *
 * Nothing can be unmounted.
 */

#ifndef MOUNT_H
#define MOUNT_H

#include <stdint.h>
#include <stddef.h>

#define MOUNT_MAX 16

/**
 * @brief Record that `source` (a device or a description) of filesystem
 *        `type` is attached at `path`. The strings must stay valid (pass
 *        string constants).
 * @return 0, or -1 if the table is full or the path is already registered.
 */
int mount_register(const char *path, const char *type, const char *source);

/**
 * @brief Number of entries.
 */
size_t mount_count(void);

/**
 * @brief Write the table as text, one "source path type" line per entry, as
 *        /proc/mounts shows it.
 * @return Length written.
 */
size_t mount_describe(char *buf, size_t size);

/**
 * @brief Print the table to the console (the kernel shell's `mount`).
 */
void vfs_list_mounts(void);

#endif // MOUNT_H
