/**
 * @file virtqueue.c
 * @brief Virtqueues (see virtqueue.h)
 */

#include "virtqueue.h"
#include "io.h"
#include "pmm.h"
#include "kstring.h"

#define DESC_F_NEXT  1 // The chain continues at `next`
#define DESC_F_WRITE 2 // The device writes to this buffer (otherwise it only reads it)

#define QUEUE_ALIGN 4096

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;      // Where the driver will put the next entry (only ever increases)
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;      // Where the device will put the next entry
    struct vring_used_elem ring[];
} __attribute__((packed));

bool virtqueue_setup(virtqueue_t *vq, uint16_t io_base, uint16_t index) {
    outw(io_base + VIRTIO_REG_QUEUE_SELECT, index);
    uint16_t size = inw(io_base + VIRTIO_REG_QUEUE_SIZE);
    if (size == 0 || size > 32768) return false; // 0: the device has no such queue

    // Layout fixed by the specification: descriptors, then the available
    // ring, then padding to a page boundary, then the used ring
    size_t avail_offset = sizeof(struct vring_desc) * size;
    size_t used_offset = (avail_offset + sizeof(uint16_t) * (3 + size) + QUEUE_ALIGN - 1) & ~(size_t)(QUEUE_ALIGN - 1);
    size_t total = used_offset + sizeof(uint16_t) * 3 + sizeof(struct vring_used_elem) * size;
    size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    void *phys = pmm_alloc_frames(pages);
    if (!phys) return false;
    if (((uint64_t)phys >> 12) > 0xFFFFFFFFULL) { // The register holds a 32-bit page number
        pmm_free_frames(phys, pages);
        return false;
    }

    uint8_t *memory = (uint8_t *)((uint64_t)phys + pmm_get_hhdm_offset());
    memset(memory, 0, pages * PAGE_SIZE);

    vq->io_base = io_base;
    vq->index = index;
    vq->size = size;
    vq->desc = (struct vring_desc *)memory;
    vq->avail = (struct vring_avail *)(memory + avail_offset);
    vq->used = (volatile struct vring_used *)(memory + used_offset);
    vq->last_used = 0;

    // All descriptors start out free, chained through their `next` fields
    for (uint16_t i = 0; i < size; i++) vq->desc[i].next = (uint16_t)(i + 1);
    vq->free_head = 0;
    vq->free_count = size;

    outl(io_base + VIRTIO_REG_QUEUE_ADDRESS, (uint32_t)((uint64_t)phys >> 12));
    return true;
}

int virtqueue_add(virtqueue_t *vq, const virtqueue_buffer_t *buffers, unsigned out_count, unsigned in_count) {
    unsigned count = out_count + in_count;
    if (count == 0 || count > VIRTQUEUE_MAX_CHAIN || count > vq->free_count) return -1;

    // Take `count` descriptors off the free list and link them into a chain
    uint16_t head = vq->free_head;
    uint16_t index = head;
    for (unsigned i = 0; i < count; i++) {
        struct vring_desc *d = &vq->desc[index];
        uint16_t next_free = d->next;

        d->addr = buffers[i].phys;
        d->len = buffers[i].len;
        d->flags = (uint16_t)((i >= out_count ? DESC_F_WRITE : 0) | (i + 1 < count ? DESC_F_NEXT : 0));
        // d->next already points at the following descriptor of the free
        // list, which is the one the chain uses next

        if (i + 1 == count) vq->free_head = next_free;
        index = next_free;
    }
    vq->free_count = (uint16_t)(vq->free_count - count);

    // Publish it. The index must be written last: it is what makes the
    // entry visible to the device.
    vq->avail->ring[vq->avail->idx % vq->size] = head;
    asm volatile("mfence" : : : "memory");
    vq->avail->idx = (uint16_t)(vq->avail->idx + 1);
    asm volatile("mfence" : : : "memory");
    return head;
}

void virtqueue_kick(virtqueue_t *vq) {
    outw(vq->io_base + VIRTIO_REG_QUEUE_NOTIFY, vq->index);
}

bool virtqueue_has_used(const virtqueue_t *vq) {
    return vq->used->idx != vq->last_used;
}

bool virtqueue_collect(virtqueue_t *vq, uint16_t *out_id, uint32_t *out_len) {
    if (!virtqueue_has_used(vq)) return false;

    uint32_t id = vq->used->ring[vq->last_used % vq->size].id;
    uint32_t len = vq->used->ring[vq->last_used % vq->size].len;
    vq->last_used = (uint16_t)(vq->last_used + 1);

    // Return the chain's descriptors to the free list. The id comes from
    // the device: do not follow it anywhere if it is out of range.
    if (id < vq->size) {
        uint16_t index = (uint16_t)id;
        unsigned freed = 1;
        while ((vq->desc[index].flags & DESC_F_NEXT) && freed < VIRTQUEUE_MAX_CHAIN && vq->desc[index].next < vq->size) {
            index = vq->desc[index].next;
            freed++;
        }
        vq->desc[index].next = vq->free_head;   // The chain's last descriptor leads back to the old free list
        vq->free_head = (uint16_t)id;
        vq->free_count = (uint16_t)(vq->free_count + freed);
    }

    if (out_id) *out_id = (uint16_t)id;
    if (out_len) *out_len = len;
    return true;
}
