/**
 * @file virtio_blk.h
 * @brief virtio block device (disk) driver
 *
 * Drives the disk QEMU provides with `-drive file=...,if=virtio`, through the
 * legacy (I/O port) virtio PCI interface. One request is in flight at a
 * time; the caller sleeps until the device signals completion.
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VIRTIO_BLK_SECTOR_SIZE 512

/**
 * @brief Find and initialise the disk.
 * @return 0 if a disk is ready, -1 if there is none or it could not be set up.
 */
int virtio_blk_init(void);

bool virtio_blk_present(void);

/**
 * @brief Size of the disk in 512-byte sectors (0 if there is no disk).
 */
uint64_t virtio_blk_capacity(void);

/**
 * @brief Read or write whole sectors. `buf` is kernel memory of
 *        count * 512 bytes. May sleep.
 * @return 0 on success, -1 on a device error, an out-of-range request, or
 *         if there is no disk.
 */
int virtio_blk_read(uint64_t lba, void *buf, size_t count);
int virtio_blk_write(uint64_t lba, const void *buf, size_t count);

/**
 * @brief Number of requests completed, and of interrupts taken, since boot.
 */
uint64_t virtio_blk_requests(void);
uint64_t virtio_blk_interrupts(void);

/**
 * @brief The most requests the device has had at one time since boot.
 */
uint32_t virtio_blk_peak_in_flight(void);

#endif // VIRTIO_BLK_H
