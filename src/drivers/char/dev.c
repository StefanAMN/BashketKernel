/**
 * @file dev.c
 * @brief Device nodes under /dev
 *
 * These are VFS nodes with no stored contents: reading or writing one calls a
 * function here (see vnode_ops_t in vfs.h).
 */

#include "dev.h"
#include "random.h"
#include "vfs.h"
#include "tty.h"
#include "kprintf.h"
#include "kstring.h"
#include "spinlock.h"
#include "virtio_blk.h"
#include "bcache.h"
#include "kerrno.h"
#include "cpu.h"
#include "mount.h"
#include "console.h"
#include "mouse.h"

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

// /dev/random: as many random bytes as asked for (never blocks)
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

// (Writing it: the screen and the serial line)
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
            if (bcache_read(lba, sector, 1) != 0) return done ? (int64_t)done : -EIO;
        }
        if (write) {
            memcpy(sector + within, buf + done, chunk);
            if (bcache_write(lba, sector, 1) != 0) return done ? (int64_t)done : -EIO;
        } else {
            memcpy(buf + done, sector + within, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

// The read and write operations of /dev/vda (see disk_transfer)
static int64_t disk_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node;
    return disk_transfer(offset, buf, count, false);
}

// (See disk_read)
static int64_t disk_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    (void)node;
    return disk_transfer(offset, (uint8_t *)buf, count, true);
}

// /dev/fb0: the screen as a file of pixels, row after row, four bytes each
// (blue, green, red, unused), `pitch` bytes per row (SYS_FB_INFO gives the
// numbers). Opening it for writing takes the screen away from the console
// until it is closed: see console_graphics_begin.
static int64_t fb_transfer(size_t offset, void *buf, size_t count, bool write) {
    uint8_t *pixels;
    uint32_t height, pitch;
    if (!console_framebuffer(&pixels, NULL, &height, &pitch)) return -ENODEV;
    size_t size = (size_t)pitch * height;
    if (offset >= size) return write ? -ENOSPC : 0;
    if (count > size - offset) count = size - offset;
    if (write) memcpy(pixels + offset, buf, count);
    else memcpy(buf, pixels + offset, count);
    return (int64_t)count;
}

// The read and write operations of /dev/fb0 (see fb_transfer)
static int64_t fb_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node;
    return fb_transfer(offset, buf, count, false);
}

// (See fb_read)
static int64_t fb_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    (void)node;
    return fb_transfer(offset, (void *)buf, count, true);
}

// Opening /dev/fb0 for writing takes the screen from the console: refused
// with -ENODEV when there is no copy of the text to put back afterwards
static int fb_opened(vnode_t *node, bool writable) {
    (void)node;
    if (writable && !console_graphics_begin()) return -ENODEV;
    return 0;
}

// The last writer gone, the console gets the screen back
static void fb_closed(vnode_t *node, bool writable) {
    (void)node;
    if (writable) console_graphics_end();
}

// /dev/mouse: movements and button changes, three bytes each (mouse.h)
static int64_t dev_mouse_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    (void)node; (void)offset;
    return mouse_read(buf, count, true);
}

// poll() on /dev/mouse and /dev/console: what their drivers say
static int dev_mouse_poll(vnode_t *node) {
    (void)node;
    return mouse_poll();
}

// (See dev_mouse_poll)
static int dev_console_poll(vnode_t *node) {
    (void)node;
    return tty_poll();
}

static const vnode_ops_t fb_ops = { .read = fb_read, .write = fb_write, .opened = fb_opened, .closed = fb_closed };
static const vnode_ops_t mouse_ops = { .read = dev_mouse_read, .poll = dev_mouse_poll };
static const vnode_ops_t disk_ops = { .read = disk_read, .write = disk_write };
static const vnode_ops_t null_ops    = { .read = NULL,             .write = null_write };
static const vnode_ops_t zero_ops    = { .read = zero_read,        .write = null_write };
static const vnode_ops_t random_ops  = { .read = random_read,      .write = null_write };
static const vnode_ops_t console_ops = { .read = dev_console_read, .write = dev_console_write, .poll = dev_console_poll };

bool dev_is_console(const vnode_t *node) {
    return node && node->ops == &console_ops;
}

void dev_init(void) {
    vfs_init();
    vnode_t *dev_dir = vfs_find(NULL, "/dev");
    if (!dev_dir || !dev_dir->mount_point) {
        dev_dir = NULL;
        vfs_mount_dir(vfs_get_root(), "dev", &dev_dir);
    }
    if (!dev_dir) return;

    mount_register("/dev", "dev", "kernel");

    // Each call is a no-op if the node already exists. The harmless devices
    // are open to everyone; the raw disk is root's alone, since writing to it
    // bypasses every permission of the filesystem on it.
    static const struct { const char *name; const vnode_ops_t *ops; uint16_t mode; } devices[] = {
        { "null", &null_ops, 0666 }, { "zero", &zero_ops, 0666 },
        { "random", &random_ops, 0666 }, { "console", &console_ops, 0666 },
    };
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        vnode_t *node = vfs_create_device(dev_dir, devices[i].name, devices[i].ops);
        if (node) node->mode = devices[i].mode;
    }
    if (virtio_blk_present()) {
        vnode_t *disk = vfs_create_device(dev_dir, "vda", &disk_ops);
        if (disk) {
            disk->size = virtio_blk_capacity() * VIRTIO_BLK_SECTOR_SIZE; // So lseek(SEEK_END) works
            disk->mode = 0600;
        }
    }
    // The screen and the mouse are for whoever sits at the machine, which
    // this kernel has no notion of: any user's programs may use them
    uint32_t height, pitch;
    bool have_fb = console_framebuffer(NULL, NULL, &height, &pitch);
    if (have_fb) {
        vnode_t *fb = vfs_create_device(dev_dir, "fb0", &fb_ops);
        if (fb) {
            fb->size = (size_t)pitch * height;
            fb->mode = 0666;
        }
    }
    if (mouse_present()) {
        vnode_t *mouse = vfs_create_device(dev_dir, "mouse", &mouse_ops);
        if (mouse) mouse->mode = 0444;
    }
    kprintf("Dev Init: /dev/null, /dev/zero, /dev/random, /dev/console%s%s%s\n",
            virtio_blk_present() ? ", /dev/vda" : "", have_fb ? ", /dev/fb0" : "", mouse_present() ? ", /dev/mouse" : "");
}
