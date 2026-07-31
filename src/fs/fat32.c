/**
 * @file fat32.c
 * @brief FAT32 filesystem (see fat32.h)
 *
 * The on-disk format in brief:
 *
 *   sector 0            boot sector: sizes and positions of everything else
 *   reserved sectors
 *   FAT (two copies)    one 32-bit entry per cluster: the number of the next
 *                       cluster of the same file, or an end-of-chain mark
 *   data area           clusters, numbered from 2
 *
 * A file is a chain of clusters; its directory entry gives the first one and
 * the size in bytes. A directory is a file whose contents are 32-byte
 * entries, one per child.
 *
 * Changing the volume means keeping three things in step: the data clusters,
 * the FAT (every copy of it), and the directory entry. They are written in
 * that order, so that a crash part-way through loses the newest data but
 * never leaves a directory entry pointing at clusters that hold something
 * else. There is no journal: a crash can leave clusters marked used that no
 * file owns ("lost clusters"), which fsck.fat repairs.
 *
 * Changes take several disk operations, each of which sleeps, so one lock
 * covers every operation that modifies the volume. Code that holds it never
 * touches user memory (which could fault) and its disk waits cannot be cut
 * short by a kill, so the lock is always released. Reads take no lock: a
 * read racing with a write can return stale or mixed data, but every cluster
 * number is range-checked, so it cannot read outside the volume.
 */

#include "fat32.h"
#include "virtio_blk.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "spinlock.h"
#include "kerrno.h"
#include "heap.h"
#include "sched.h"

#define SECTOR VIRTIO_BLK_SECTOR_SIZE

#define FAT_ENTRY_MASK 0x0FFFFFFFu   // The top four bits of an entry are reserved
#define FAT_END        0x0FFFFFF8u   // This value or above: end of chain
#define FAT_END_MARK   0x0FFFFFFFu   // What we write to end a chain
#define FAT_FREE       0u

#define ATTR_ARCHIVE   0x20          // Set on files that have been written

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LONG_NAME 0x0F          // An entry holding part of a long file name

#define NT_LOWER_BASE 0x08           // Show the name part in lower case
#define NT_LOWER_EXT  0x10           // Show the extension in lower case

#define MAX_DEPTH 8                  // Directory nesting followed
#define MAX_DIR_ENTRIES 4096         // Entries read from one directory
#define MAX_WRITE_BYTES (64 * 1024)  // Most that one write call transfers
#define FAT_DATE_FIXED (((2026 - 1980) << 9) | (1 << 5) | 1) // 2026-01-01: no clock is consulted

struct dir_entry {
    uint8_t  name[11];               // 8 characters of name, 3 of extension, space padded
    uint8_t  attr;
    uint8_t  nt_flags;
    uint8_t  create_tenths;
    uint16_t create_time, create_date, access_date;
    uint16_t cluster_high;
    uint16_t write_time, write_date;
    uint16_t cluster_low;
    uint32_t size;
} __attribute__((packed));

// What the driver remembers about each file and directory of the volume
// (vnode->private_data points to one of these)
typedef struct {
    uint32_t first_cluster;    // 0 for an empty file
    uint64_t entry_lba;        // Sector holding its directory entry (0 for the root, which has none)
    uint32_t entry_offset;     // Byte offset of the entry in that sector
} fat_node_t;

static const vnode_ops_t fat_file_ops;
static const vnode_ops_t fat_dir_ops;

static fat32_layout_t volume; // Not "fs": with Intel-syntax assembly that name is read as the FS register
static bool mounted = false;

// One sector of the FAT is kept in memory: following a chain reads the same
// sector many times over.
static bool volume_locked = false;    // See the comment at the top about the lock
static uint32_t alloc_hint = 2;       // Where the search for a free cluster starts
static bool fsinfo_invalidated = false;

static uint8_t fat_cache[SECTOR];
static uint32_t fat_cache_sector = 0; // 0: nothing cached (sector 0 is never part of the FAT)

static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t read32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool fat32_parse_boot_sector(const uint8_t *sector, uint64_t disk_sectors, fat32_layout_t *out) {
    if (!sector || !out) return false;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return false;       // Boot sector signature

    uint32_t bytes_per_sector = read16(sector + 11);
    uint32_t sectors_per_cluster = sector[13];
    uint32_t reserved = read16(sector + 14);
    uint32_t fat_count = sector[16];
    uint32_t root_entries = read16(sector + 17);                        // 0 on FAT32
    uint32_t total_sectors_16 = read16(sector + 19);                    // 0 on FAT32
    uint32_t fat_sectors_16 = read16(sector + 22);                      // 0 on FAT32
    uint32_t total_sectors = read32(sector + 32);
    uint32_t fat_sectors = read32(sector + 36);
    uint32_t root_cluster = read32(sector + 44);

    if (bytes_per_sector != SECTOR) return false;                       // The only size the disk driver speaks
    if (sectors_per_cluster == 0 || sectors_per_cluster > 128) return false;
    if (sectors_per_cluster & (sectors_per_cluster - 1)) return false;  // Must be a power of two
    if (reserved == 0 || fat_count == 0 || fat_count > 4) return false;
    if (root_entries != 0 || total_sectors_16 != 0 || fat_sectors_16 != 0) return false; // Those mean FAT12/16
    if (fat_sectors == 0 || total_sectors == 0) return false;
    if ((uint64_t)total_sectors > disk_sectors) return false;           // Claims to be bigger than the disk

    uint64_t data_start = (uint64_t)reserved + (uint64_t)fat_count * fat_sectors;
    if (data_start >= total_sectors) return false;

    uint64_t cluster_count = (total_sectors - data_start) / sectors_per_cluster;
    if (cluster_count < 65525) return false;                            // Fewer clusters would be FAT16, by definition
    if (cluster_count + 2 > (uint64_t)fat_sectors * (SECTOR / 4)) return false; // The FAT is too short to describe them
    if (root_cluster < 2 || root_cluster >= cluster_count + 2) return false;

    out->sectors_per_cluster = sectors_per_cluster;
    out->fat_count = fat_count;
    out->fsinfo_sector = read16(sector + 48);
    out->fat_start = reserved;
    out->fat_sectors = fat_sectors;
    out->data_start = (uint32_t)data_start;
    out->cluster_count = (uint32_t)cluster_count;
    out->root_cluster = root_cluster;
    return true;
}

static bool valid_cluster(uint32_t cluster) {
    return cluster >= 2 && cluster < volume.cluster_count + 2;
}

static uint64_t cluster_to_sector(uint32_t cluster) {
    return (uint64_t)volume.data_start + (uint64_t)(cluster - 2) * volume.sectors_per_cluster;
}

// The cluster after `cluster` in its chain. Returns 0 at the end of the
// chain, on a read error, or if the entry is not a usable cluster number.
static uint32_t next_cluster(uint32_t cluster) {
    if (!valid_cluster(cluster)) return 0;

    uint32_t sector = volume.fat_start + cluster / (SECTOR / 4);
    uint32_t index = cluster % (SECTOR / 4);
    uint32_t entry;

    uint64_t flags = irq_save();
    bool cached = (fat_cache_sector == sector);
    if (cached) entry = read32(fat_cache + index * 4);
    irq_restore(flags);

    if (!cached) {
        // Read into a private buffer: the disk read sleeps, and another task
        // may use the shared cache meanwhile
        uint8_t fresh[SECTOR];
        if (virtio_blk_read(sector, fresh, 1) != 0) return 0;
        entry = read32(fresh + index * 4);

        flags = irq_save();
        memcpy(fat_cache, fresh, SECTOR);
        fat_cache_sector = sector;
        irq_restore(flags);
    }

    entry &= FAT_ENTRY_MASK;
    if (entry >= FAT_END) return 0;
    return valid_cluster(entry) ? entry : 0; // Free, bad or out-of-range: stop here
}

// Read from a cluster chain. `limit` is the length of the file in bytes.
// Returns the number of bytes read, which is less than asked for only at the
// end of the file or if the chain ends early (a damaged volume).
static int64_t read_chain(uint32_t first_cluster, uint64_t limit, size_t offset, void *buf, size_t count) {
    if (offset >= limit) return 0;
    if (count > limit - offset) count = (size_t)(limit - offset);

    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR;
    uint8_t *out = buf;
    size_t done = 0;

    // Walk to the cluster that contains `offset`
    uint32_t cluster = first_cluster;
    for (size_t skip = offset / cluster_bytes; skip > 0 && cluster; skip--) {
        cluster = next_cluster(cluster);
    }

    size_t within = offset % cluster_bytes; // Position inside the current cluster
    uint32_t steps = 0;
    while (done < count && valid_cluster(cluster)) {
        uint8_t sector[SECTOR];
        uint64_t lba = cluster_to_sector(cluster) + within / SECTOR;
        if (virtio_blk_read(lba, sector, 1) != 0) return done ? (int64_t)done : -EIO;

        size_t in_sector = within % SECTOR;
        size_t chunk = SECTOR - in_sector;
        if (chunk > count - done) chunk = count - done;
        memcpy(out + done, sector + in_sector, chunk);
        done += chunk;
        within += chunk;

        if (within == cluster_bytes) {
            within = 0;
            cluster = next_cluster(cluster);
            if (++steps > volume.cluster_count) break; // A chain longer than the volume is a loop
        }
    }
    return (int64_t)done;
}

// --- Locking ---------------------------------------------------------------

static void volume_lock(void) {
    uint64_t flags = irq_save();
    while (volume_locked) sched_block_uninterruptible(&volume_locked, 0);
    volume_locked = true;
    irq_restore(flags);
}

static void volume_unlock(void) {
    uint64_t flags = irq_save();
    volume_locked = false;
    sched_wakeup(&volume_locked);
    irq_restore(flags);
}

// --- Changing the FAT (all callers hold the volume lock) ---------------------

// The FSInfo sector caches the number of free clusters. We do not keep it up
// to date, so before the first change it is set to "unknown", which the
// format allows; otherwise checkers would report it as wrong.
static void invalidate_fsinfo(void) {
    if (fsinfo_invalidated) return;
    fsinfo_invalidated = true;

    uint8_t sector[SECTOR];
    uint32_t lba = volume.fsinfo_sector;
    if (lba == 0 || lba >= volume.fat_start) return;
    if (virtio_blk_read(lba, sector, 1) != 0) return;
    if (read32(sector) != 0x41615252 || read32(sector + 484) != 0x61417272) return; // Not an FSInfo sector
    memset(sector + 488, 0xFF, 8); // Free count and next-free hint: unknown
    virtio_blk_write(lba, sector, 1);
}

static bool set_fat_entry(uint32_t cluster, uint32_t value) {
    if (!valid_cluster(cluster)) return false;
    invalidate_fsinfo();

    uint32_t offset_sector = cluster / (SECTOR / 4);
    uint32_t index = cluster % (SECTOR / 4);
    uint8_t sector[SECTOR];

    if (virtio_blk_read(volume.fat_start + offset_sector, sector, 1) != 0) return false;

    // The top four bits of an entry are reserved and must be preserved
    uint32_t entry = (read32(sector + index * 4) & ~FAT_ENTRY_MASK) | (value & FAT_ENTRY_MASK);
    sector[index * 4 + 0] = (uint8_t)entry;
    sector[index * 4 + 1] = (uint8_t)(entry >> 8);
    sector[index * 4 + 2] = (uint8_t)(entry >> 16);
    sector[index * 4 + 3] = (uint8_t)(entry >> 24);

    // Every copy of the FAT gets the same change
    for (uint32_t copy = 0; copy < volume.fat_count; copy++) {
        uint32_t lba = volume.fat_start + copy * volume.fat_sectors + offset_sector;
        if (virtio_blk_write(lba, sector, 1) != 0) return false;
    }

    uint64_t flags = irq_save();
    if (fat_cache_sector == volume.fat_start + offset_sector) memcpy(fat_cache, sector, SECTOR);
    irq_restore(flags);
    return true;
}

static bool zero_cluster(uint32_t cluster) {
    uint8_t zeros[SECTOR];
    memset(zeros, 0, SECTOR);
    for (uint32_t s = 0; s < volume.sectors_per_cluster; s++) {
        if (virtio_blk_write(cluster_to_sector(cluster) + s, zeros, 1) != 0) return false;
    }
    return true;
}

// Find a free cluster, mark it as the end of a chain, and return it (0 if
// the volume is full or the disk fails).
static uint32_t allocate_cluster(void) {
    uint32_t total = volume.cluster_count;
    uint8_t sector[SECTOR];
    uint32_t loaded = 0; // FAT sector currently in `sector` (0: none)

    for (uint32_t n = 0; n < total; n++) {
        uint32_t cluster = 2 + (alloc_hint - 2 + n) % total;
        uint32_t lba = volume.fat_start + cluster / (SECTOR / 4);
        if (lba != loaded) {
            if (virtio_blk_read(lba, sector, 1) != 0) return 0;
            loaded = lba;
        }
        if ((read32(sector + (cluster % (SECTOR / 4)) * 4) & FAT_ENTRY_MASK) == FAT_FREE) {
            if (!set_fat_entry(cluster, FAT_END_MARK)) return 0;
            alloc_hint = cluster + 1;
            return cluster;
        }
    }
    return 0;
}

// Give back every cluster of a chain.
static void free_chain(uint32_t cluster) {
    uint32_t steps = 0;
    while (valid_cluster(cluster) && steps++ <= volume.cluster_count) {
        uint32_t next = next_cluster(cluster);
        set_fat_entry(cluster, FAT_FREE);
        if (cluster < alloc_hint) alloc_hint = cluster;
        cluster = next;
    }
}

// --- Directory entries on disk ------------------------------------------------

// Change the first cluster and size recorded in a node's directory entry.
static bool update_entry(const fat_node_t *info, uint32_t first_cluster, uint32_t size) {
    if (info->entry_lba == 0) return true; // The root directory has no entry

    uint8_t sector[SECTOR];
    if (virtio_blk_read(info->entry_lba, sector, 1) != 0) return false;

    struct dir_entry *entry = (struct dir_entry *)(sector + info->entry_offset);
    entry->cluster_high = (uint16_t)(first_cluster >> 16);
    entry->cluster_low = (uint16_t)first_cluster;
    entry->size = size;
    return virtio_blk_write(info->entry_lba, sector, 1) == 0;
}

// "notes.txt" -> "NOTES   TXT" plus the lower-case flags. FAT short names are
// upper case; a name that is all lower case is stored upper case with a flag
// saying "show it in lower case". Mixed case would need a long file name
// entry, which this driver does not write.
static bool encode_name(const char *name, uint8_t out[11], uint8_t *nt_flags) {
    static const char allowed[] = "0123456789_-~!#$%&'()@^`{}";
    size_t base_len = 0, ext_len = 0;
    bool in_ext = false;
    bool base_lower = false, base_upper = false, ext_lower = false, ext_upper = false;

    memset(out, ' ', 11);
    for (const char *p = name; *p != '\0'; p++) {
        char c = *p;
        if (c == '.') {
            if (in_ext || base_len == 0) return false; // A second dot, or no name before it
            in_ext = true;
            continue;
        }

        bool lower = (c >= 'a' && c <= 'z');
        bool upper = (c >= 'A' && c <= 'Z');
        if (!lower && !upper && !strchr(allowed, c)) return false;
        if (lower) c = (char)(c - 32);

        if (in_ext) {
            if (ext_len >= 3) return false;
            out[8 + ext_len++] = (uint8_t)c;
            ext_lower |= lower;
            ext_upper |= upper;
        } else {
            if (base_len >= 8) return false;
            out[base_len++] = (uint8_t)c;
            base_lower |= lower;
            base_upper |= upper;
        }
    }

    if (base_len == 0) return false;
    if ((base_lower && base_upper) || (ext_lower && ext_upper)) return false; // Mixed case
    *nt_flags = (uint8_t)((base_lower ? NT_LOWER_BASE : 0) | (ext_lower ? NT_LOWER_EXT : 0));
    return true;
}

// Find an unused 32-byte slot in a directory, extending the directory by a
// cluster if it is full. Returns where the slot is.
static bool find_free_slot(fat_node_t *dir, uint64_t *out_lba, uint32_t *out_offset) {
    uint32_t cluster = dir->first_cluster;
    uint32_t last = 0;
    uint32_t steps = 0;
    uint8_t sector[SECTOR];

    while (valid_cluster(cluster) && steps++ <= volume.cluster_count) {
        for (uint32_t s = 0; s < volume.sectors_per_cluster; s++) {
            uint64_t lba = cluster_to_sector(cluster) + s;
            if (virtio_blk_read(lba, sector, 1) != 0) return false;
            for (uint32_t offset = 0; offset < SECTOR; offset += sizeof(struct dir_entry)) {
                // 0x00: never used (and nothing follows); 0xE5: deleted
                if (sector[offset] == 0x00 || sector[offset] == 0xE5) {
                    *out_lba = lba;
                    *out_offset = offset;
                    return true;
                }
            }
        }
        last = cluster;
        cluster = next_cluster(cluster);
    }
    if (!last) return false;

    // Every slot is taken: add a cluster of empty slots to the directory
    uint32_t added = allocate_cluster();
    if (!added) return false;
    if (!zero_cluster(added) || !set_fat_entry(last, added)) {
        set_fat_entry(added, FAT_FREE);
        return false;
    }
    *out_lba = cluster_to_sector(added);
    *out_offset = 0;
    return true;
}

static void fill_entry(struct dir_entry *entry, const uint8_t name[11], uint8_t nt_flags, uint8_t attr, uint32_t cluster) {
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, name, 11);
    entry->attr = attr;
    entry->nt_flags = nt_flags;
    entry->create_date = entry->access_date = entry->write_date = FAT_DATE_FIXED;
    entry->cluster_high = (uint16_t)(cluster >> 16);
    entry->cluster_low = (uint16_t)cluster;
}

// --- File operations ----------------------------------------------------------

static int64_t fat_file_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    const fat_node_t *info = (const fat_node_t *)node->private_data;
    return read_chain(info->first_cluster, node->size, offset, buf, count);
}

// Write `count` bytes from kernel memory at `offset`, growing the file as
// needed. Caller holds the volume lock.
static int64_t write_locked(vnode_t *node, size_t offset, const uint8_t *data, size_t count) {
    fat_node_t *info = (fat_node_t *)node->private_data;
    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR;
    size_t end = offset + count;
    size_t done = 0;

    // Walk (and where needed build) the chain, one cluster per iteration,
    // from the start of the file to the last cluster the write touches
    uint32_t cluster = info->first_cluster;
    uint32_t previous = 0;
    bool first_cluster_changed = false;

    for (size_t position = 0; position < end; position += cluster_bytes) {
        if (!valid_cluster(cluster)) {
            // The chain ends before here: add a cluster
            uint32_t added = allocate_cluster();
            if (!added) break; // Volume full (or disk error): stop with what is written so far

            // If this write does not cover the whole new cluster, the rest
            // of it must read as zeros, not as whatever was there before
            bool fully_covered = (offset <= position && end >= position + cluster_bytes);
            if (!fully_covered && !zero_cluster(added)) { set_fat_entry(added, FAT_FREE); break; }

            if (previous) {
                if (!set_fat_entry(previous, added)) { set_fat_entry(added, FAT_FREE); break; }
            } else {
                info->first_cluster = added;
                first_cluster_changed = true;
            }
            cluster = added;
        }

        // The part of this cluster that the write covers, if any
        size_t from = offset > position ? offset - position : 0;
        size_t to = end - position < cluster_bytes ? end - position : cluster_bytes;
        bool failed = false;
        while (position + cluster_bytes > offset && from < to) {
            uint64_t lba = cluster_to_sector(cluster) + from / SECTOR;
            size_t in_sector = from % SECTOR;
            size_t chunk = SECTOR - in_sector;
            if (chunk > to - from) chunk = to - from;

            uint8_t sector[SECTOR];
            if (chunk < SECTOR && virtio_blk_read(lba, sector, 1) != 0) { failed = true; break; } // Keep the rest of the sector
            memcpy(sector + in_sector, data + done, chunk);
            if (virtio_blk_write(lba, sector, 1) != 0) { failed = true; break; }

            done += chunk;
            from += chunk;
        }
        if (failed) break;

        previous = cluster;
        cluster = next_cluster(cluster);
    }

    // Record the new size (and first cluster) in the directory entry last
    size_t reached = (done > 0) ? offset + done : 0;
    if (reached > node->size || first_cluster_changed) {
        size_t new_size = reached > node->size ? reached : node->size;
        if (!update_entry(info, info->first_cluster, (uint32_t)new_size)) return -EIO;
        node->size = new_size;
    }

    if (done == 0 && count > 0) return -ENOSPC;
    return (int64_t)done;
}

static int64_t fat_file_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    if (count == 0) return 0;
    if (count > MAX_WRITE_BYTES) count = MAX_WRITE_BYTES;          // A short write; the caller continues
    if (offset > 0xFFFFFFFFu || count > 0xFFFFFFFFu - offset) return -ENOSPC; // FAT32 files end at 4 GiB

    // `buf` may be user memory, and touching that can fault. Copy it into
    // kernel memory first, so that nothing done under the lock can.
    uint8_t *copy = kmalloc(count);
    if (!copy) return -ENOMEM;
    memcpy(copy, buf, count);

    volume_lock();
    int64_t result = write_locked(node, offset, copy, count);
    volume_unlock();

    kfree(copy);
    return result;
}

static int fat_file_truncate(vnode_t *node) {
    fat_node_t *info = (fat_node_t *)node->private_data;

    volume_lock();
    uint32_t chain = info->first_cluster;
    // Directory entry first: a crash in between leaves lost clusters, which
    // is harmless, rather than an entry pointing at freed ones
    bool ok = update_entry(info, 0, 0);
    if (ok) {
        info->first_cluster = 0;
        node->size = 0;
        free_chain(chain);
    }
    volume_unlock();
    return ok ? 0 : -EIO;
}

// --- Directory operations -----------------------------------------------------

static int fat_dir_create(vnode_t *dir, const char *name, bool directory, vnode_t **out) {
    fat_node_t *dir_info = (fat_node_t *)dir->private_data;
    uint8_t short_name[11];
    uint8_t nt_flags;
    if (!encode_name(name, short_name, &nt_flags)) return -EINVAL; // Not an 8.3 name

    fat_node_t *info = kmalloc(sizeof(fat_node_t));
    if (!info) return -ENOMEM;
    memset(info, 0, sizeof(*info));

    int err = 0;
    volume_lock();

    // A new directory needs a cluster of its own straight away, holding its
    // "." and ".." entries
    if (directory) {
        uint32_t cluster = allocate_cluster();
        uint8_t sector[SECTOR];
        memset(sector, 0, SECTOR);
        static const uint8_t dot[11]    = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
        static const uint8_t dotdot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
        // ".." of a directory in the root is recorded as cluster 0
        uint32_t parent_cluster = (dir_info->entry_lba == 0) ? 0 : dir_info->first_cluster;
        fill_entry((struct dir_entry *)sector, dot, 0, ATTR_DIRECTORY, cluster);
        fill_entry((struct dir_entry *)(sector + sizeof(struct dir_entry)), dotdot, 0, ATTR_DIRECTORY, parent_cluster);

        if (!cluster) {
            err = -ENOSPC;
        } else if (!zero_cluster(cluster) || virtio_blk_write(cluster_to_sector(cluster), sector, 1) != 0) {
            set_fat_entry(cluster, FAT_FREE);
            err = -EIO;
        } else {
            info->first_cluster = cluster;
        }
    }

    // Then the entry in the parent
    if (err == 0) {
        uint8_t sector[SECTOR];
        if (!find_free_slot(dir_info, &info->entry_lba, &info->entry_offset)) {
            err = -ENOSPC;
        } else if (virtio_blk_read(info->entry_lba, sector, 1) != 0) {
            err = -EIO;
        } else {
            fill_entry((struct dir_entry *)(sector + info->entry_offset), short_name, nt_flags,
                       directory ? ATTR_DIRECTORY : ATTR_ARCHIVE, info->first_cluster);
            if (virtio_blk_write(info->entry_lba, sector, 1) != 0) err = -EIO;
        }
        if (err != 0 && info->first_cluster) free_chain(info->first_cluster);
    }

    volume_unlock();

    vnode_t *node = (err == 0) ? vfs_attach(dir, name, directory ? VFS_DIRECTORY : VFS_FILE) : NULL;
    if (!node) {
        // (If only the attach failed, the entry exists on disk and will show
        // up at the next mount; that needs the kernel to be out of memory.)
        kfree(info);
        return err ? err : -ENOMEM;
    }
    node->ops = directory ? &fat_dir_ops : &fat_file_ops;
    node->private_data = (uint64_t)info;
    *out = node;
    return 0;
}

static int fat_dir_remove(vnode_t *dir, vnode_t *child) {
    (void)dir;
    fat_node_t *info = (fat_node_t *)child->private_data;
    uint8_t sector[SECTOR];
    int err = 0;

    volume_lock();
    if (virtio_blk_read(info->entry_lba, sector, 1) != 0) {
        err = -EIO;
    } else {
        sector[info->entry_offset] = 0xE5; // The mark for a deleted entry
        if (virtio_blk_write(info->entry_lba, sector, 1) != 0) err = -EIO;
    }
    if (err == 0) free_chain(info->first_cluster);
    volume_unlock();

    if (err == 0) {
        kfree(info);
        child->private_data = 0;
    }
    return err;
}

static const vnode_ops_t fat_file_ops = { .read = fat_file_read, .write = fat_file_write, .truncate = fat_file_truncate };
static const vnode_ops_t fat_dir_ops = { .create = fat_dir_create, .remove = fat_dir_remove };

// --- Mounting -----------------------------------------------------------------

// "README  TXT" + flags -> "readme.txt". Returns false for names that cannot
// be shown (control characters, or characters that mean something in a path).
static bool decode_name(const struct dir_entry *entry, char *out) {
    size_t len = 0;

    for (int i = 0; i < 11; i++) {
        uint8_t c = entry->name[i];
        if (i == 0 && c == 0x05) c = 0xE5;          // 0x05 stands for a real 0xE5 first byte
        if (c == ' ') continue;                     // Padding
        if (c < 0x21 || c > 0x7E || c == '/' || c == '\\') return false;

        if (i == 8) out[len++] = '.';               // The extension starts here
        bool lower = (i < 8) ? (entry->nt_flags & NT_LOWER_BASE) : (entry->nt_flags & NT_LOWER_EXT);
        if (lower && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        out[len++] = (char)c;
    }

    out[len] = '\0';
    return len > 0;
}

// Attach a node for one directory entry. Returns NULL if memory ran out.
static vnode_t *attach_entry(vnode_t *dir, const char *name, bool directory, uint32_t first_cluster,
                             uint32_t size, uint64_t entry_lba, uint32_t entry_offset) {
    if (vfs_find_child(dir, name)) return NULL; // Two entries with one name: keep the first

    fat_node_t *info = kmalloc(sizeof(fat_node_t));
    if (!info) return NULL;
    info->first_cluster = first_cluster;
    info->entry_lba = entry_lba;
    info->entry_offset = entry_offset;

    vnode_t *node = vfs_attach(dir, name, directory ? VFS_DIRECTORY : VFS_FILE);
    if (!node) {
        kfree(info);
        return NULL;
    }
    node->ops = directory ? &fat_dir_ops : &fat_file_ops;
    node->private_data = (uint64_t)info;
    node->size = size;
    return node;
}

// Add the contents of the directory starting at `cluster` to the VFS
// directory `dir`, recursively. Returns the number of files added.
static int read_directory(uint32_t cluster, vnode_t *dir, int depth) {
    int files = 0;
    int entries_seen = 0;
    uint32_t steps = 0;

    while (valid_cluster(cluster) && steps++ <= volume.cluster_count) {
        for (uint32_t s = 0; s < volume.sectors_per_cluster; s++) {
            uint8_t sector[SECTOR];
            uint64_t lba = cluster_to_sector(cluster) + s;
            if (virtio_blk_read(lba, sector, 1) != 0) return files;

            for (uint32_t offset = 0; offset < SECTOR; offset += sizeof(struct dir_entry)) {
                const struct dir_entry *entry = (const struct dir_entry *)(sector + offset);

                if (entry->name[0] == 0x00) return files;               // No entries after this one
                if (++entries_seen > MAX_DIR_ENTRIES) return files;
                if (entry->name[0] == 0xE5) continue;                   // Deleted
                if ((entry->attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) continue; // Part of a long name
                if (entry->attr & ATTR_VOLUME_ID) continue;             // The volume's label
                if (entry->name[0] == '.') continue;                    // "." and ".."

                char name[13];
                if (!decode_name(entry, name)) continue;

                uint32_t first = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;

                if (entry->attr & ATTR_DIRECTORY) {
                    if (depth >= MAX_DEPTH || !valid_cluster(first)) continue;
                    vnode_t *child = attach_entry(dir, name, true, first, 0, lba, offset);
                    if (child) files += read_directory(first, child, depth + 1);
                } else {
                    // An empty file has no cluster; anything else needs a valid one
                    if (entry->size > 0 && !valid_cluster(first)) continue;
                    if (attach_entry(dir, name, false, entry->size ? first : 0, entry->size, lba, offset)) files++;
                }
            }
        }
        cluster = next_cluster(cluster);
    }
    return files;
}

int fat32_mount(const char *mount_point) {
    if (mounted || !mount_point) return -1;
    if (!virtio_blk_present()) return -1;

    uint8_t boot[SECTOR];
    if (virtio_blk_read(0, boot, 1) != 0) return -1;
    if (!fat32_parse_boot_sector(boot, virtio_blk_capacity(), &volume)) {
        kprintf("fat32: the disk does not hold a FAT32 volume this driver can read\n");
        return -1;
    }

    const char *leaf = NULL;
    vnode_t *parent = vfs_find_parent(mount_point, &leaf);
    vnode_t *root = parent ? vfs_create_dir(parent, leaf) : NULL;
    if (!root || root->children || root->ops) return -1; // Must be a new or empty in-memory directory

    fat_node_t *root_info = kmalloc(sizeof(fat_node_t));
    if (!root_info) return -1;
    root_info->first_cluster = volume.root_cluster;
    root_info->entry_lba = 0; // The root directory has no entry of its own
    root_info->entry_offset = 0;
    root->ops = &fat_dir_ops;
    root->private_data = (uint64_t)root_info;

    mounted = true;
    int files = read_directory(volume.root_cluster, root, 0);

    kprintf("fat32: mounted at %s: %d files, %u clusters of %u bytes\n",
            mount_point, files, volume.cluster_count, volume.sectors_per_cluster * SECTOR);
    return files;
}

bool fat32_mounted(void) {
    return mounted;
}

uint32_t fat32_free_clusters(void) {
    if (!mounted) return 0;

    uint32_t free_count = 0;
    uint8_t sector[SECTOR];
    for (uint32_t cluster = 2; cluster < volume.cluster_count + 2; cluster++) {
        if (cluster == 2 || cluster % (SECTOR / 4) == 0) {
            if (virtio_blk_read(volume.fat_start + cluster / (SECTOR / 4), sector, 1) != 0) return free_count;
        }
        if ((read32(sector + (cluster % (SECTOR / 4)) * 4) & FAT_ENTRY_MASK) == FAT_FREE) free_count++;
    }
    return free_count;
}
