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
    kprintf("Dev Init: /dev/null, /dev/zero, /dev/random, /dev/console\n");
}
