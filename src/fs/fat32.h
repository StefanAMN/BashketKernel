/**
 * @file fat32.h
 * @brief FAT32 filesystem, read-only
 *
 * Reads a FAT32 volume from the virtio disk and presents its directories and
 * files under a mount point in the VFS. File contents stay on the disk and
 * are read when asked for. Nothing can be created, changed or removed.
 *
 * Supported: short (8.3) names, subdirectories, any cluster size. Long file
 * name entries are skipped, so a file with a long name appears under its
 * short alias.
 *
 * Everything read from the disk is treated as untrusted: sizes and cluster
 * numbers are range-checked and cluster chains are length-limited, so a
 * damaged or hostile disk image cannot make the kernel read outside the
 * volume or loop for ever.
 */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Where the pieces of the volume are, worked out from the boot sector
typedef struct {
    uint32_t sectors_per_cluster;
    uint32_t fat_start;        // First sector of the file allocation table
    uint32_t fat_sectors;      // Length of one copy of the table
    uint32_t data_start;       // First sector of cluster 2
    uint32_t cluster_count;    // Number of data clusters; valid numbers are 2 .. cluster_count + 1
    uint32_t root_cluster;     // First cluster of the root directory
} fat32_layout_t;

/**
 * @brief Decide whether a 512-byte sector is the boot sector of a FAT32
 *        volume this driver can read, and if so work out its layout.
 * @param disk_sectors Size of the disk, to check that the volume fits on it.
 */
bool fat32_parse_boot_sector(const uint8_t *sector, uint64_t disk_sectors, fat32_layout_t *out);

/**
 * @brief Mount the disk's FAT32 volume at `mount_point` (created if needed).
 * @return Number of files found, or -1 if there is no disk, it does not
 *         hold a FAT32 volume, or the mount point cannot be used.
 */
int fat32_mount(const char *mount_point);

bool fat32_mounted(void);

#endif // FAT32_H
