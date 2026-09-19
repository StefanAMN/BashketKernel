/**
 * @file virtio_blk.c
 * @brief Implementation of VirtIO Block Device Driver
 */

#include "virtio_blk.h"
#include "kprintf.h"
#include "heap.h"
#include "kstring.h"

#define SIMULATED_SECTORS 2048

static uint8_t *storage_disk = NULL;
static bool initialized = false;

int virtio_blk_init(void) {
    if (initialized) return 0;

    // Allocate 1 MB simulated storage disk buffer (2048 sectors)
    storage_disk = (uint8_t *)kmalloc(SIMULATED_SECTORS * VIRTIO_BLK_SECTOR_SIZE);
    if (!storage_disk) {
        kprintf("VirtIO-Blk: ERROR - Failed to allocate block storage buffer!\n");
        return -1;
    }

    memset(storage_disk, 0, SIMULATED_SECTORS * VIRTIO_BLK_SECTOR_SIZE);
    initialized = true;

    kprintf("virtio-blk: STUB, a %u-sector RAM buffer (%u KB); no real device is driven\n",
            SIMULATED_SECTORS, (SIMULATED_SECTORS * VIRTIO_BLK_SECTOR_SIZE) / 1024);

    return 0;
}

int virtio_blk_read_sector(uint64_t lba, uint8_t *buf) {
    if (!initialized || !buf || lba >= SIMULATED_SECTORS) return -1;
    memcpy(buf, storage_disk + (lba * VIRTIO_BLK_SECTOR_SIZE), VIRTIO_BLK_SECTOR_SIZE);
    return 0;
}

int virtio_blk_write_sector(uint64_t lba, const uint8_t *buf) {
    if (!initialized || !buf || lba >= SIMULATED_SECTORS) return -1;
    memcpy(storage_disk + (lba * VIRTIO_BLK_SECTOR_SIZE), buf, VIRTIO_BLK_SECTOR_SIZE);
    return 0;
}
