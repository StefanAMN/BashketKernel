/**
 * @file bcache.c
 * @brief A cache of disk sectors (see bcache.h)
 */

#include "bcache.h"
#include "virtio_blk.h"
#include "spinlock.h"
#include "heap.h"
#include "kstring.h"

#define SECTOR VIRTIO_BLK_SECTOR_SIZE

typedef struct {
    bool valid;
    uint8_t dev;        // Which disk
    uint64_t lba;
} entry_t;

static entry_t entries[BCACHE_SECTORS];
static uint8_t *data = NULL;        // BCACHE_SECTORS sectors, allocated on first use
static bcache_stats_t stats;

// Counts writes. A reader that fetched a sector from the disk only puts it
// in the cache if no write has happened since it started: otherwise it
// might be putting back what a writer has just replaced.
static uint64_t write_count = 0;

// Protects all of the above. Never held across a disk request.
static spinlock_t cache_lock = SPINLOCK_RANKED("bcache", LOCK_RANK_BCACHE);

static bool ready(void) {
    if (data) return true;
    uint8_t *memory = kmalloc((size_t)BCACHE_SECTORS * SECTOR);
    if (!memory) return false;

    uint64_t flags = spinlock_acquire_irqsave(&cache_lock);
    bool ours = (data == NULL);
    if (ours) data = memory;
    spinlock_release_irqrestore(&cache_lock, flags);
    if (!ours) kfree(memory); // Someone else got there first
    return true;
}

// Caller holds cache_lock
static void store(unsigned dev, uint64_t lba, const uint8_t *sector) {
    size_t slot = (size_t)(lba % BCACHE_SECTORS);
    entries[slot].valid = true;
    entries[slot].dev = (uint8_t)dev;
    entries[slot].lba = lba;
    memcpy(data + slot * SECTOR, sector, SECTOR);
}

int bcache_read(uint64_t lba, void *buf, size_t count) { return bcache_read_dev(0, lba, buf, count); }
int bcache_write(uint64_t lba, const void *buf, size_t count) { return bcache_write_dev(0, lba, buf, count); }

int bcache_read_dev(unsigned dev, uint64_t lba, void *buf, size_t count) {
    if (!ready()) return virtio_blk_read_dev(dev, lba, buf, count); // No memory for a cache: do without

    uint8_t *out = buf;
    for (size_t i = 0; i < count; i++, out += SECTOR) {
        size_t slot = (size_t)((lba + i) % BCACHE_SECTORS);

        uint64_t flags = spinlock_acquire_irqsave(&cache_lock);
        bool hit = entries[slot].valid && entries[slot].dev == dev && entries[slot].lba == lba + i;
        if (hit) {
            memcpy(out, data + slot * SECTOR, SECTOR);
            stats.hits++;
        } else {
            stats.misses++;
        }
        uint64_t writes_before = write_count;
        spinlock_release_irqrestore(&cache_lock, flags);
        if (hit) continue;

        if (virtio_blk_read_dev(dev, lba + i, out, 1) != 0) return -1;

        flags = spinlock_acquire_irqsave(&cache_lock);
        if (write_count == writes_before) store(dev, lba + i, out);
        spinlock_release_irqrestore(&cache_lock, flags);
    }
    return 0;
}

int bcache_write_dev(unsigned dev, uint64_t lba, const void *buf, size_t count) {
    // The disk first: the cache must never hold what the disk does not
    int result = virtio_blk_write_dev(dev, lba, buf, count);
    if (!data) return result;

    const uint8_t *in = buf;
    uint64_t flags = spinlock_acquire_irqsave(&cache_lock);
    write_count++;
    stats.writes += count;
    for (size_t i = 0; i < count; i++, in += SECTOR) {
        size_t slot = (size_t)((lba + i) % BCACHE_SECTORS);
        if (result == 0) {
            store(dev, lba + i, in);
        } else if (entries[slot].valid && entries[slot].dev == dev && entries[slot].lba == lba + i) {
            entries[slot].valid = false; // What the disk holds now is anyone's guess
        }
    }
    spinlock_release_irqrestore(&cache_lock, flags);
    return result;
}

void bcache_invalidate(void) {
    uint64_t flags = spinlock_acquire_irqsave(&cache_lock);
    memset(entries, 0, sizeof(entries));
    write_count++;
    spinlock_release_irqrestore(&cache_lock, flags);
}

void bcache_get_stats(bcache_stats_t *out) {
    uint64_t flags = spinlock_acquire_irqsave(&cache_lock);
    *out = stats;
    spinlock_release_irqrestore(&cache_lock, flags);
}
