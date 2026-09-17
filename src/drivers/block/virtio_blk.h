/**
 * @file virtio_blk.h
 * @brief virtio block device (disk) driver
 *
 * Drives the disks QEMU provides with `-drive file=...,if=virtio` (up to
 * VIRTIO_BLK_MAX_DEVICES), through whichever virtio interface each offers
 * (virtio_pci.h). Up to eight requests per disk are with the device at
 * once; each caller sleeps until the device signals that its request is
 * done.
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VIRTIO_BLK_SECTOR_SIZE 512
#define VIRTIO_BLK_MAX_DEVICES 3   // Disks the driver will take charge of

/**
 * @brief Find and set up every virtio disk (numbered in the order found),
 *        and route their interrupts. Called once at boot.
 * @return 0 if at least one disk is ready, -1 if there is none.
 */
int virtio_blk_init(void);

/**
 * @brief Whether there is at least one disk.
 */
bool virtio_blk_present(void);

/**
 * @brief Size of disk 0 in 512-byte sectors (0 if there is no disk).
 */
uint64_t virtio_blk_capacity(void);

/**
 * @brief Read `count` whole sectors of disk 0 from `lba` into the kernel
 *        buffer `buf` (count * 512 bytes). Sleeps, uninterruptibly, until
 *        the device answers (or 5 s pass).
 * @return 0 on success, -1 on a device error, an out-of-range request, a
 *         timeout, or if there is no disk.
 */
int virtio_blk_read(uint64_t lba, void *buf, size_t count);

/**
 * @brief Write `count` whole sectors to disk 0 (as virtio_blk_read).
 */
int virtio_blk_write(uint64_t lba, const void *buf, size_t count);

/**
 * @brief virtio_blk_read for disk number `dev`.
 */
int virtio_blk_read_dev(unsigned dev, uint64_t lba, void *buf, size_t count);

/**
 * @brief virtio_blk_write for disk number `dev`.
 */
int virtio_blk_write_dev(unsigned dev, uint64_t lba, const void *buf, size_t count);

/**
 * @brief How many disks were set up.
 */
unsigned virtio_blk_count(void);

/**
 * @brief Size of disk `dev` in sectors (0 if there is no such disk).
 */
uint64_t virtio_blk_capacity_of(unsigned dev);

/**
 * @brief Requests disk `dev` has completed since boot (0 for a disk that
 *        does not exist). Per disk, so that a measurement on one is not
 *        disturbed by the system disk being read at the same time.
 */
uint64_t virtio_blk_requests(unsigned dev);

/**
 * @brief Interrupts disk `dev` has raised since boot (as above).
 */
uint64_t virtio_blk_interrupts(unsigned dev);

/**
 * @brief How disk `dev` interrupts: true if by MSI-X, with the vector and
 *        the CPU it is sent to; false if on its line (*line set to the IRQ).
 *        Any pointer may be NULL.
 */
bool virtio_blk_interrupt_route(unsigned dev, uint8_t *vector, uint32_t *cpu, uint8_t *line);

/**
 * @brief The most requests any disk has had at one time since boot.
 */
uint32_t virtio_blk_peak_in_flight(void);

#endif // VIRTIO_BLK_H
