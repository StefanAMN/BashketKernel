/**
 * @file virtio_blk.h
 * @brief VirtIO Block Device Driver Interface
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VIRTIO_BLK_SECTOR_SIZE 512

/**
 * @brief Initialize VirtIO Block device.
 * @return 0 on success, negative error code on failure.
 */
int virtio_blk_init(void);

/**
 * @brief Read a single 512-byte sector from block device.
 * @param lba Logical Block Address sector number.
 * @param buf Output buffer of at least 512 bytes.
 * @return 0 on success, negative error code on failure.
 */
int virtio_blk_read_sector(uint64_t lba, uint8_t *buf);

/**
 * @brief Write a single 512-byte sector to block device.
 * @param lba Logical Block Address sector number.
 * @param buf Input buffer of 512 bytes.
 * @return 0 on success, negative error code on failure.
 */
int virtio_blk_write_sector(uint64_t lba, const uint8_t *buf);

#endif // VIRTIO_BLK_H
