/**
 * @file dev.c
 * @brief Device nodes under /dev
 *
 * These are VFS nodes with no stored contents: reading or writing one calls a
 * function here (see vnode_ops_t in vfs.h).
 */

#include "dev.h"
#include "vfs.h"
#include "tty.h"
#include "kprintf.h"
#include "kstring.h"
#include "spinlock.h"
#include "virtio_blk.h"
#include "kerrno.h"

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// --- Random numbers ---------------------------------------------------------

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL; // Never zero: xorshift would stick there

static uint64_t rng_next(void) {
    uint64_t flags = irq_save();
    // Stir in the timestamp counter so the stream depends on when it is read
    uint64_t x = rng_state ^ rdtsc();
    if (x == 0) x = 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    irq_restore(flags);
    return x * 0x2545F4914F6CDD1DULL;
}

uint32_t get_random_u32(void) {
    return (uint32_t)(rng_next() >> 32);
}

void get_random_bytes(void *buf, size_t count) {
    uint8_t *out = buf;
    while (count > 0) {
        uint64_t value = rng_next();
        size_t chunk = count < sizeof(value) ? count : sizeof(value);
        memcpy(out, &value, chunk);
        out += chunk;
        count -= chunk;
    }
}

// --- Device operations -------------------------------------------------------

// /dev/null: reads see end-of-file, writes are accepted and thrown away
static int64_t null_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    (void)node; (void)offset; (void)buf;
    return (int64_t)count;
}

// /dev/zero: an endless supply of zero bytes
static int64_t zero_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node; (void)offset;
    memset(buf, 0, count);
    return (int64_t)count;
}

static int64_t random_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node; (void)offset;
    get_random_bytes(buf, count);
    return (int64_t)count;
}

// /dev/console: the terminal, whatever standard I/O was redirected to
static int64_t dev_console_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node; (void)offset;
    return tty_read(buf, count);
}

static int64_t dev_console_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    (void)node; (void)offset;
    return tty_write(buf, count);
}

// /dev/vda: the whole disk as one long file. Reads and writes may start and
// end anywhere; the disk itself only deals in whole 512-byte sectors, so a
// partial sector is read, changed and written back.
static int64_t disk_transfer(size_t offset, uint8_t *buf, size_t count, bool write) {
    uint64_t disk_bytes = virtio_blk_capacity() * VIRTIO_BLK_SECTOR_SIZE;
    if (!virtio_blk_present()) return -ENODEV;
    if (offset >= disk_bytes) return write ? -ENOSPC : 0;   // Past the end
    if (count > disk_bytes - offset) count = (size_t)(disk_bytes - offset);

    uint8_t sector[VIRTIO_BLK_SECTOR_SIZE];
    size_t done = 0;
    while (done < count) {
        uint64_t lba = (offset + done) / VIRTIO_BLK_SECTOR_SIZE;
        size_t within = (offset + done) % VIRTIO_BLK_SECTOR_SIZE;
        size_t chunk = VIRTIO_BLK_SECTOR_SIZE - within;
        if (chunk > count - done) chunk = count - done;

        bool whole_sector = (chunk == VIRTIO_BLK_SECTOR_SIZE);
        if (!write || !whole_sector) {
            if (virtio_blk_read(lba, sector, 1) != 0) return done ? (int64_t)done : -EIO;
        }
        if (write) {
            memcpy(sector + within, buf + done, chunk);
            if (virtio_blk_write(lba, sector, 1) != 0) return done ? (int64_t)done : -EIO;
        } else {
            memcpy(buf + done, sector + within, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t disk_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node;
    return disk_transfer(offset, buf, count, false);
}

static int64_t disk_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    (void)node;
    return disk_transfer(offset, (uint8_t *)buf, count, true);
}

static const vnode_ops_t disk_ops = { .read = disk_read, .write = disk_write };
static const vnode_ops_t null_ops    = { .read = NULL,             .write = null_write };
static const vnode_ops_t zero_ops    = { .read = zero_read,        .write = null_write };
static const vnode_ops_t random_ops  = { .read = random_read,      .write = null_write };
static const vnode_ops_t console_ops = { .read = dev_console_read, .write = dev_console_write };

void dev_init(void) {
    vfs_init();
    vnode_t *dev_dir = vfs_create_dir(vfs_get_root(), "dev");
    if (!dev_dir) return;

    rng_state ^= rdtsc();

    // Each call is a no-op if the node already exists
    vfs_create_device(dev_dir, "null", &null_ops);
    vfs_create_device(dev_dir, "zero", &zero_ops);
    vfs_create_device(dev_dir, "random", &random_ops);
    vfs_create_device(dev_dir, "console", &console_ops);
    if (virtio_blk_present()) {
        vnode_t *disk = vfs_create_device(dev_dir, "vda", &disk_ops);
        if (disk) disk->size = virtio_blk_capacity() * VIRTIO_BLK_SECTOR_SIZE; // So lseek(SEEK_END) works
    }
    kprintf("Dev Init: /dev/null, /dev/zero, /dev/random, /dev/console%s\n",
            virtio_blk_present() ? ", /dev/vda" : "");
}
