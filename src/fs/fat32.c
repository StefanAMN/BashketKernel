/**
 * @file fat32.c
 * @brief FAT32 filesystem, read-only (see fat32.h)
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
 */

#include "fat32.h"
#include "virtio_blk.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "spinlock.h"
#include "kerrno.h"

#define SECTOR VIRTIO_BLK_SECTOR_SIZE

#define FAT_ENTRY_MASK 0x0FFFFFFFu   // The top four bits of an entry are reserved
#define FAT_END        0x0FFFFFF8u   // This value or above: end of chain

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LONG_NAME 0x0F          // An entry holding part of a long file name

#define NT_LOWER_BASE 0x08           // Show the name part in lower case
#define NT_LOWER_EXT  0x10           // Show the extension in lower case

#define MAX_DEPTH 8                  // Directory nesting followed
#define MAX_DIR_ENTRIES 4096         // Entries read from one directory

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

static fat32_layout_t volume; // Not "fs": with Intel-syntax assembly that name is read as the FS register
static bool mounted = false;

// One sector of the FAT is kept in memory: following a chain reads the same
// sector many times over.
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

static int64_t fat_file_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    return read_chain((uint32_t)node->private_data, node->size, offset, buf, count);
}

static const vnode_ops_t fat_file_ops = { .read = fat_file_read, .write = NULL };

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

// Add the contents of the directory starting at `cluster` to the VFS
// directory `dir`, recursively. Returns the number of files added.
static int read_directory(uint32_t cluster, vnode_t *dir, int depth) {
    int files = 0;
    int entries_seen = 0;
    uint32_t steps = 0;

    while (valid_cluster(cluster) && steps++ <= volume.cluster_count) {
        for (uint32_t s = 0; s < volume.sectors_per_cluster; s++) {
            uint8_t sector[SECTOR];
            if (virtio_blk_read(cluster_to_sector(cluster) + s, sector, 1) != 0) return files;

            for (size_t i = 0; i < SECTOR / sizeof(struct dir_entry); i++) {
                const struct dir_entry *entry = (const struct dir_entry *)(sector + i * sizeof(struct dir_entry));

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
                    vnode_t *child = vfs_create_dir(dir, name);
                    if (!child) continue;
                    files += read_directory(first, child, depth + 1);
                    child->read_only = true; // Only once its entries have been created
                } else {
                    // An empty file has no cluster; anything else needs a valid one
                    if (entry->size > 0 && !valid_cluster(first)) continue;
                    vnode_t *file = vfs_create_device(dir, name, &fat_file_ops);
                    if (!file) continue;
                    file->size = entry->size;
                    file->private_data = first;
                    file->read_only = true;
                    files++;
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
    if (!root || root->children) return -1; // Must be a new or empty directory

    mounted = true;
    int files = read_directory(volume.root_cluster, root, 0);
    root->read_only = true; // After filling it: the directory entries themselves are created by us

    kprintf("fat32: mounted at %s, read-only: %d files, %u clusters of %u bytes\n",
            mount_point, files, volume.cluster_count, volume.sectors_per_cluster * SECTOR);
    return files;
}

bool fat32_mounted(void) {
    return mounted;
}
