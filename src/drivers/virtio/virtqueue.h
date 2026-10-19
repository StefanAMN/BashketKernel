/**
 * @file virtqueue.h
 * @brief Virtqueues: the shared-memory rings every virtio device uses
 *
 * A virtqueue is how a driver hands buffers to a virtio device and gets them
 * back (virtio 1.2 specification, section 2.7, "split virtqueues"). It is
 * three arrays in ordinary memory whose addresses the device is told (how
 * it is told differs between the two register interfaces: virtio_pci.h):
 *
 *   descriptor table   each entry describes one buffer: physical address,
 *                      length, whether the device may write to it, and
 *                      optionally the index of the next buffer of the same
 *                      request
 *   available ring     written by the driver: "the request starting at
 *                      descriptor N is ready for you"
 *   used ring          written by the device: "request N is done, and I
 *                      wrote this many bytes"
 *
 * A request is a chain of descriptors: first the buffers the device only
 * reads, then the ones it writes. The driver adds a chain, notifies the
 * device ("kick"), and later collects it from the used ring.
 *
 * Buffers are named by physical address, because the device accesses them
 * directly (DMA). They must be frames from the frame allocator, not heap
 * memory.
 *
 * Nothing here locks: each driver has a spinlock for its device and holds
 * it around every call that changes a queue.
 */

#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "virtio_pci.h"

// Registers of the legacy virtio PCI interface, as offsets from the I/O base
#define VIRTIO_REG_DEVICE_FEATURES 0x00 // 32 bits, read
#define VIRTIO_REG_DRIVER_FEATURES 0x04 // 32 bits, write
#define VIRTIO_REG_QUEUE_ADDRESS   0x08 // 32 bits: physical page number of the selected queue
#define VIRTIO_REG_QUEUE_SIZE      0x0C // 16 bits, read
#define VIRTIO_REG_QUEUE_SELECT    0x0E // 16 bits
#define VIRTIO_REG_QUEUE_NOTIFY    0x10 // 16 bits
#define VIRTIO_REG_DEVICE_STATUS   0x12 // 8 bits
#define VIRTIO_REG_ISR_STATUS      0x13 // 8 bits; reading it acknowledges the interrupt
#define VIRTIO_REG_DEVICE_CONFIG   0x14 // Device-specific fields start here

#define VIRTQUEUE_MAX_CHAIN 4          // Longest chain of buffers in one request

struct vring_desc;
struct vring_avail;
struct vring_used;

typedef struct {
    virtio_device_t *dev;              // The device this queue belongs to
    volatile uint16_t *notify_address; // Modern interface: where to write to notify the device
    uint16_t index;                    // Which of the device's queues this is
    uint16_t size;                     // Number of descriptors (the device decides)
    struct vring_desc *desc;
    struct vring_avail *avail;
    volatile struct vring_used *used;
    uint16_t last_used;                // How much of the used ring has been collected
    uint16_t free_head;                // First descriptor of the free list
    uint16_t free_count;
} virtqueue_t;

// One buffer of a request
typedef struct {
    uint64_t phys;
    uint32_t len;
} virtqueue_buffer_t;

/**
 * @brief Set up queue `index` of a device: ask its size, allocate and clear
 *        the rings, and tell the device where they are.
 * @return false if the device has no such queue or memory ran out.
 */
bool virtqueue_setup(virtqueue_t *vq, virtio_device_t *dev, uint16_t index);

/**
 * @brief For virtqueue_setup (the step that depends on the register
 *        interface, in virtio_pci.c): how many entries queue `index` has
 *        (0: the device has no such queue).
 */
uint16_t virtio_queue_size(virtio_device_t *dev, uint16_t index);

/**
 * @brief For virtqueue_setup: tell the device where the three parts of
 *        queue `index` are and enable it. *notify_address receives where
 *        to write to kick it with the modern interface (NULL: legacy, an
 *        I/O port). False if the legacy interface cannot reach the memory
 *        or the notification address is outside what was mapped.
 */
bool virtio_queue_start(virtio_device_t *dev, uint16_t index, uint64_t desc_phys, uint64_t avail_phys, uint64_t used_phys,
                        volatile uint16_t **notify_address);

/**
 * @brief Add one request: `out_count` buffers the device reads, followed by
 *        `in_count` buffers it writes.
 * @return The request's id (its first descriptor), for matching it later,
 *         or -1 if there are not enough free descriptors.
 */
int virtqueue_add(virtqueue_t *vq, const virtqueue_buffer_t *buffers, unsigned out_count, unsigned in_count);

/**
 * @brief Tell the device that requests have been added.
 */
void virtqueue_kick(virtqueue_t *vq);

/**
 * @brief Collect one finished request, releasing its descriptors.
 * @param out_id Receives the id virtqueue_add returned.
 * @param out_len Receives the number of bytes the device wrote.
 * @return false if nothing has finished.
 */
bool virtqueue_collect(virtqueue_t *vq, uint16_t *out_id, uint32_t *out_len);

/**
 * @brief True if a finished request is waiting to be collected.
 */
bool virtqueue_has_used(const virtqueue_t *vq);

#endif // VIRTQUEUE_H
