/**
 * @file fat32.h
 * @brief FAT32 File System Driver Interface
 */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize FAT32 File System on primary block device.
 * @return 0 on success, negative error code on failure.
 */
int fat32_init(void);

/**
 * @brief Create a file on the FAT32 file system.
 * @param filename 11-char FAT filename (e.g. "TEST    TXT").
 * @param data File payload bytes.
 * @param len Data length in bytes.
 * @return 0 on success, negative error code on failure.
 */
int fat32_write_file(const char *filename, const uint8_t *data, size_t len);

/**
 * @brief Read a file from the FAT32 file system.
 * @param filename FAT filename.
 * @param out_buf Buffer to store file data.
 * @param max_len Capacity of output buffer.
 * @return Bytes read on success, negative error code on failure.
 */
int fat32_read_file(const char *filename, uint8_t *out_buf, size_t max_len);

#endif // FAT32_H
