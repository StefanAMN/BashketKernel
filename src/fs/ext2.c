/**
 * @file ext2.c
 * @brief The ext2 filesystem (see ext2.h)
 *
 * The on-disk format in brief (all numbers little-endian; "block" means a
 * filesystem block of 1, 2 or 4 KiB, not a disk sector):
 *
 *   byte 1024          superblock: sizes, counts, and where things are
 *   next block         group descriptors: one per block group
 *   then, per group:   a bitmap of its blocks, a bitmap of its inodes, the
 *                      inode table, and data blocks
 *
 * An inode is a file's record: type and permission bits, owner, group,
 * size, times, a link count, and where its contents are: twelve block
 * numbers directly, then one block full of block numbers ("indirect"), then
 * one block of numbers of such blocks ("doubly indirect"). A directory is a
 * file whose contents are records of (inode number, length of this record,
 * name). A file's name is not in its inode: it is whatever the directories
 * call it. Inode 2 is the root directory.
 *
 * Every change is written to the disk as it is made, in an order that keeps
 * a crash from leaving a name pointing at nothing: a new file's inode is
 * written before the directory entry that names it; on removal the entry
 * goes first. What a crash can leave behind is blocks or inodes marked
 * used that nothing refers to, and free counts that are off, both of which
 * e2fsck repairs. There is no journal.
 *
 * Everything read from the disk is treated as possibly wrong: block and
 * inode numbers are checked against the size of the volume, record lengths
 * against the block they are in.
 */

#include "ext2.h"
#include "bcache.h"
#include "virtio_blk.h"
#include "vfs.h"
#include "mount.h"
#include "mutex.h"
#include "heap.h"
#include "rtc.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerrno.h"

#define SECTOR VIRTIO_BLK_SECTOR_SIZE
#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INO 2
#define MAX_BLOCK_SIZE 4096
#define DIRECT_BLOCKS 12
#define NAME_MAX_LEN 127             // What a vnode's name can hold (ext2 itself allows 255)

// Features. An "incompatible" feature we do not know means we would
// misread the volume; a "read-only compatible" one means we would damage
// it by writing. Either way: do not mount.
#define INCOMPAT_FILETYPE   0x0002   // Directory entries carry the file's type
#define RO_COMPAT_SPARSE    0x0001   // Superblock copies only in some groups (we never touch the copies)
#define RO_COMPAT_LARGE     0x0002   // Files may be 4 GiB or more (we refuse those files, not the volume)

#define MODE_TYPE_MASK 0xF000
#define MODE_REGULAR   0x8000
#define MODE_DIRECTORY 0x4000
#define FT_REGULAR 1                 // File types as stored in directory entries
#define FT_DIRECTORY 2

#define INODE_FLAG_INDEX 0x1000      // The directory has a hash index (which we do not maintain)

struct ext2_inode {
    uint16_t mode;                   // Type in the top four bits, permission bits below
    uint16_t uid;                    // Low 16 bits of the owner
    uint32_t size;
    uint32_t atime, ctime, mtime, dtime; // Seconds since 1970; dtime: when it was deleted
    uint16_t gid;
    uint16_t links;                  // Directory entries naming this inode
    uint32_t sectors;                // 512-byte units in use, indirect blocks included
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];              // 12 direct, 1 indirect, 1 doubly, 1 triply indirect
    uint32_t generation, file_acl, size_high, faddr;
    uint16_t frag, uid_high_pad;     // (Layout of the Linux variant)
    uint16_t uid_high, gid_high;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct ext2_inode) == 128, "an ext2 inode is 128 bytes");

struct ext2_group {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, pad;
    uint8_t reserved[12];
} __attribute__((packed));

_Static_assert(sizeof(struct ext2_group) == 32, "a group descriptor is 32 bytes");

// What the driver remembers about each file and directory (vnode->private_data)
typedef struct {
    uint32_t ino;
} ext2_node_t;

static struct {
    unsigned disk;
    uint32_t block_size, sectors_per_block, pointers_per_block;
    uint32_t inodes_count, blocks_count, first_data_block;
    uint32_t blocks_per_group, inodes_per_group, inode_size, first_ino, groups;
    uint32_t free_blocks, free_inodes;
    uint32_t group_table_block;      // Where the group descriptors start
} vol; // Not "fs": with Intel-syntax assembly that name is read as the FS register

static bool mounted = false;
static struct ext2_group *groups = NULL;     // The group descriptors, kept in memory
static uint8_t superblock[1024];             // And the superblock as read, to write back with new counts

// One lock for the whole volume, held by every operation below: they share
// the scratch blocks that follow, and a change is many blocks long.
static mutex_t ext2_mutex = MUTEX_INIT("ext2");

// Scratch blocks, one per purpose so that the functions that nest (writing
// a file allocates a block, which reads a bitmap) do not overwrite each
// other's. Too big for the kernel stack.
static uint8_t bitmap_block[MAX_BLOCK_SIZE];
static uint8_t inode_block[MAX_BLOCK_SIZE];
static uint8_t indirect_block[MAX_BLOCK_SIZE];
static uint8_t indirect2_block[MAX_BLOCK_SIZE];
static uint8_t dir_block[MAX_BLOCK_SIZE];
static uint8_t data_block[MAX_BLOCK_SIZE];
static uint8_t zero_block[MAX_BLOCK_SIZE];

static const vnode_ops_t ext2_file_ops;
static const vnode_ops_t ext2_dir_ops;

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// --- Time ------------------------------------------------------------------------

// Days since 1970-01-01 of a date (the algorithm for the proleptic Gregorian
// calendar from Howard Hinnant's "chrono-compatible low-level date algorithms")
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint32_t unix_now(void) {
    struct rtc_time now;
    rtc_read_time(&now);
    if (now.year < 1970 || now.month < 1 || now.month > 12 || now.day < 1 || now.day > 31) return 0;
    int64_t days = days_from_civil(now.year, now.month, now.day);
    return (uint32_t)(days * 86400 + (now.hour % 24) * 3600 + (now.minute % 60) * 60 + (now.second % 60));
}

// Seconds since 1970 as the VFS keeps times (FAT's packing: see vfs.h)
static uint32_t unix_to_packed(uint32_t seconds) {
    int64_t z = (int64_t)(seconds / 86400) + 719468;
    int64_t era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    unsigned year = (unsigned)(yoe + era * 400) + (month <= 2);
    if (year < 1980) return 0; // Before FAT's calendar begins: shown as unknown
    unsigned rest = seconds % 86400;
    uint32_t date = ((year - 1980) << 9) | (month << 5) | day;
    uint32_t time = ((rest / 3600) << 11) | (((rest / 60) % 60) << 5) | ((rest % 60) / 2);
    return (date << 16) | time;
}

// --- Blocks ----------------------------------------------------------------------

static bool valid_block(uint32_t block) {
    return block >= vol.first_data_block && block < vol.blocks_count;
}

static bool read_block(uint32_t block, void *buf) {
    if (block >= vol.blocks_count) return false;
    return bcache_read_dev(vol.disk, (uint64_t)block * vol.sectors_per_block, buf, vol.sectors_per_block) == 0;
}

static bool write_block(uint32_t block, const void *buf) {
    if (block >= vol.blocks_count) return false;
    return bcache_write_dev(vol.disk, (uint64_t)block * vol.sectors_per_block, buf, vol.sectors_per_block) == 0;
}

// The counts of free blocks and inodes live in the superblock and, per
// group, in the group descriptors. Both are rewritten after every change.
static bool write_counts(uint32_t group) {
    put32(superblock + 12, vol.free_blocks);
    put32(superblock + 16, vol.free_inodes);
    if (bcache_write_dev(vol.disk, 2, superblock, 2) != 0) return false; // The superblock is at byte 1024

    // The block of the group table that holds this group's descriptor
    uint32_t per_block = vol.block_size / sizeof(struct ext2_group);
    uint32_t table_block = group / per_block;
    return write_block(vol.group_table_block + table_block, (const uint8_t *)groups + (size_t)table_block * vol.block_size);
}

// Take a free block, preferably in `goal_group` (near the file's other
// blocks). Returns 0 if the volume is full.
static uint32_t alloc_block(uint32_t goal_group) {
    for (uint32_t tried = 0; tried < vol.groups; tried++) {
        uint32_t g = (goal_group + tried) % vol.groups;
        if (groups[g].free_blocks == 0) continue;
        if (!valid_block(groups[g].block_bitmap) || !read_block(groups[g].block_bitmap, bitmap_block)) continue;

        uint32_t first = vol.first_data_block + g * vol.blocks_per_group;
        uint32_t in_group = vol.blocks_count - first < vol.blocks_per_group ? vol.blocks_count - first : vol.blocks_per_group;
        for (uint32_t bit = 0; bit < in_group; bit++) {
            if (bitmap_block[bit / 8] & (1u << (bit % 8))) continue;

            bitmap_block[bit / 8] |= (uint8_t)(1u << (bit % 8));
            if (!write_block(groups[g].block_bitmap, bitmap_block)) return 0;
            groups[g].free_blocks--;
            if (vol.free_blocks) vol.free_blocks--;
            write_counts(g);

            // Never hand out a block with someone's old contents in it
            uint32_t block = first + bit;
            if (!write_block(block, zero_block)) return 0;
            return block;
        }
        // The descriptor said there was room and the bitmap says not:
        // believe the bitmap, and look elsewhere
    }
    return 0;
}

static void free_block(uint32_t block) {
    if (!valid_block(block)) return;
    uint32_t g = (block - vol.first_data_block) / vol.blocks_per_group;
    uint32_t bit = (block - vol.first_data_block) % vol.blocks_per_group;
    if (g >= vol.groups || !valid_block(groups[g].block_bitmap) || !read_block(groups[g].block_bitmap, bitmap_block)) return;
    if (!(bitmap_block[bit / 8] & (1u << (bit % 8)))) return; // Already free: do not count it twice

    bitmap_block[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    if (!write_block(groups[g].block_bitmap, bitmap_block)) return;
    groups[g].free_blocks++;
    vol.free_blocks++;
    write_counts(g);
}

// --- Inodes ----------------------------------------------------------------------

static bool valid_inode(uint32_t ino) {
    return ino >= 1 && ino <= vol.inodes_count;
}

// Where inode `ino` is: the block of its group's inode table, and the
// offset in that block
static bool inode_location(uint32_t ino, uint32_t *block, uint32_t *offset) {
    if (!valid_inode(ino)) return false;
    uint32_t g = (ino - 1) / vol.inodes_per_group;
    uint32_t index = (ino - 1) % vol.inodes_per_group;
    if (g >= vol.groups) return false;
    uint64_t byte = (uint64_t)index * vol.inode_size;
    *block = groups[g].inode_table + (uint32_t)(byte / vol.block_size);
    *offset = (uint32_t)(byte % vol.block_size);
    return valid_block(*block);
}

static bool read_inode(uint32_t ino, struct ext2_inode *out) {
    uint32_t block, offset;
    if (!inode_location(ino, &block, &offset) || !read_block(block, inode_block)) return false;
    memcpy(out, inode_block + offset, sizeof(*out));
    return true;
}

static bool write_inode(uint32_t ino, const struct ext2_inode *inode) {
    uint32_t block, offset;
    if (!inode_location(ino, &block, &offset) || !read_block(block, inode_block)) return false;
    memcpy(inode_block + offset, inode, sizeof(*inode)); // Bytes past the first 128 of a larger inode stay as they are
    return write_block(block, inode_block);
}

// Take a free inode, preferably in the parent directory's group. Returns 0
// if there is none.
static uint32_t alloc_inode(uint32_t goal_group, bool directory) {
    for (uint32_t tried = 0; tried < vol.groups; tried++) {
        uint32_t g = (goal_group + tried) % vol.groups;
        if (groups[g].free_inodes == 0) continue;
        if (!valid_block(groups[g].inode_bitmap) || !read_block(groups[g].inode_bitmap, bitmap_block)) continue;

        for (uint32_t bit = 0; bit < vol.inodes_per_group; bit++) {
            uint32_t ino = g * vol.inodes_per_group + bit + 1;
            if (ino < vol.first_ino || ino > vol.inodes_count) continue; // The first few are reserved
            if (bitmap_block[bit / 8] & (1u << (bit % 8))) continue;

            bitmap_block[bit / 8] |= (uint8_t)(1u << (bit % 8));
            if (!write_block(groups[g].inode_bitmap, bitmap_block)) return 0;
            groups[g].free_inodes--;
            if (directory) groups[g].used_dirs++;
            if (vol.free_inodes) vol.free_inodes--;
            write_counts(g);
            return ino;
        }
    }
    return 0;
}

static void free_inode(uint32_t ino, bool directory) {
    if (!valid_inode(ino) || ino < vol.first_ino) return;
    uint32_t g = (ino - 1) / vol.inodes_per_group;
    uint32_t bit = (ino - 1) % vol.inodes_per_group;
    if (g >= vol.groups || !valid_block(groups[g].inode_bitmap) || !read_block(groups[g].inode_bitmap, bitmap_block)) return;
    if (!(bitmap_block[bit / 8] & (1u << (bit % 8)))) return;

    bitmap_block[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    if (!write_block(groups[g].inode_bitmap, bitmap_block)) return;
    groups[g].free_inodes++;
    if (directory && groups[g].used_dirs) groups[g].used_dirs--;
    vol.free_inodes++;
    write_counts(g);
}

// --- A file's blocks -----------------------------------------------------------------

// One step down the tree of block numbers: slot `index` of the block of
// numbers `table_block`, read through `scratch`. With `alloc`, an empty
// slot gets a new block. Returns 0 for "none" (or failure, with *error set).
static uint32_t table_entry(uint32_t table_block, uint32_t index, uint8_t *scratch, bool alloc,
                            uint32_t goal_group, struct ext2_inode *inode, bool *error) {
    if (!valid_block(table_block) || !read_block(table_block, scratch)) { *error = true; return 0; }
    uint32_t block = get32(scratch + index * 4);
    if (block != 0) {
        if (!valid_block(block)) { *error = true; return 0; }
        return block;
    }
    if (!alloc) return 0;

    block = alloc_block(goal_group);
    if (!block) { *error = true; return 0; }
    // alloc_block does not use `scratch`, so it still holds the table
    put32(scratch + index * 4, block);
    if (!write_block(table_block, scratch)) { *error = true; return 0; }
    inode->sectors += vol.sectors_per_block;
    return block;
}

// The disk block holding block number `index` of a file, or 0 if there is
// none there (a hole, or past the end). With `alloc`, missing blocks are
// allocated: the data block and any blocks of numbers on the way to it. The
// caller writes the inode afterwards.
static uint32_t file_block(struct ext2_inode *inode, uint32_t index, bool alloc, uint32_t goal_group, bool *error) {
    uint32_t p = vol.pointers_per_block;

    if (index < DIRECT_BLOCKS) {
        uint32_t block = inode->block[index];
        if (block == 0 && alloc) {
            block = alloc_block(goal_group);
            if (!block) { *error = true; return 0; }
            inode->block[index] = block;
            inode->sectors += vol.sectors_per_block;
        }
        if (block != 0 && !valid_block(block)) { *error = true; return 0; }
        return block;
    }
    index -= DIRECT_BLOCKS;

    // Which top-level slot of the inode, and how many levels below it
    int slot = (index < p) ? 12 : 13;
    if (slot == 13) {
        index -= p;
        if (index >= p * p) { *error = true; return 0; } // Would need the triply indirect block: too big for us
    }

    uint32_t top = inode->block[slot];
    if (top == 0) {
        if (!alloc) return 0;
        top = alloc_block(goal_group);
        if (!top) { *error = true; return 0; }
        inode->block[slot] = top;
        inode->sectors += vol.sectors_per_block;
    }

    if (slot == 12) return table_entry(top, index, indirect_block, alloc, goal_group, inode, error);

    uint32_t middle = table_entry(top, index / p, indirect2_block, alloc, goal_group, inode, error);
    if (middle == 0) return 0;
    return table_entry(middle, index % p, indirect_block, alloc, goal_group, inode, error);
}

// Free every block listed in a block of numbers, `depth` levels deep, and
// the block itself
static void free_tree(uint32_t block, int depth) {
    if (!valid_block(block)) return;
    if (depth > 0) {
        // The numbers are copied out first: freeing uses the scratch blocks
        uint32_t *numbers = kmalloc(vol.block_size);
        if (numbers && read_block(block, numbers)) {
            for (uint32_t i = 0; i < vol.pointers_per_block; i++) {
                if (numbers[i]) free_tree(numbers[i], depth - 1);
            }
        }
        kfree(numbers);
    }
    free_block(block);
}

// Give back everything a file occupies. The inode is changed in memory;
// the caller writes it.
static void free_contents(struct ext2_inode *inode) {
    for (int i = 0; i < DIRECT_BLOCKS; i++) {
        if (inode->block[i]) free_block(inode->block[i]);
    }
    if (inode->block[12]) free_tree(inode->block[12], 1);
    if (inode->block[13]) free_tree(inode->block[13], 2);
    if (inode->block[14]) free_tree(inode->block[14], 3);
    memset(inode->block, 0, sizeof(inode->block));
    inode->size = 0;
    inode->sectors = 0;
}

static uint32_t group_of_inode(uint32_t ino) {
    return (ino - 1) / vol.inodes_per_group;
}

// --- Directories -----------------------------------------------------------------
//
// A directory's blocks are each a chain of records:
//
//     inode number (4 bytes; 0: an unused record)
//     length of this record (2): where the next one starts
//     length of the name (1), type of the file (1), the name
//
// A record's length may be larger than it needs, and the last record of a
// block always reaches to the block's end. Removing an entry adds its
// space to the record before it; adding one splits a record with space to
// spare. Records never cross a block boundary.

static uint32_t record_size(size_t name_len) {
    return (uint32_t)((8 + name_len + 3) & ~(size_t)3);
}

// Is the record at `offset` of a directory block well formed?
static bool record_ok(const uint8_t *block, uint32_t offset) {
    if (offset + 8 > vol.block_size) return false;
    uint32_t length = get16(block + offset + 4);
    uint32_t name_len = block[offset + 6];
    return length >= 8 && (length & 3) == 0 && offset + length <= vol.block_size && 8 + name_len <= length;
}

// Add the entry (name -> ino) to a directory. The directory's inode is
// changed in memory if it had to grow; the caller writes it.
static int dir_add(struct ext2_inode *dir, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type) {
    size_t name_len = strlen(name);
    uint32_t needed = record_size(name_len);
    uint32_t blocks = dir->size / vol.block_size;
    bool error = false;

    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t block = file_block(dir, b, false, 0, &error);
        if (error) return -EIO;
        if (block == 0 || !read_block(block, dir_block)) continue;

        for (uint32_t offset = 0; offset < vol.block_size; ) {
            if (!record_ok(dir_block, offset)) break; // A damaged block: leave it alone
            uint32_t length = get16(dir_block + offset + 4);
            uint32_t used = get32(dir_block + offset) ? record_size(dir_block[offset + 6]) : 0;

            if (length - used >= needed) {
                // Room here: after this record's own entry if it has one,
                // else in its place
                uint32_t at = offset + used;
                if (used) put16(dir_block + offset + 4, (uint16_t)used);
                put32(dir_block + at, ino);
                put16(dir_block + at + 4, (uint16_t)(length - used));
                dir_block[at + 6] = (uint8_t)name_len;
                dir_block[at + 7] = type;
                memcpy(dir_block + at + 8, name, name_len);
                return write_block(block, dir_block) ? 0 : -EIO;
            }
            offset += length;
        }
    }

    // No room anywhere: a new block, all of it one record
    uint32_t block = file_block(dir, blocks, true, group_of_inode(dir_ino), &error);
    if (!block) return error ? -ENOSPC : -EIO;
    memset(dir_block, 0, vol.block_size);
    put32(dir_block, ino);
    put16(dir_block + 4, (uint16_t)vol.block_size);
    dir_block[6] = (uint8_t)name_len;
    dir_block[7] = type;
    memcpy(dir_block + 8, name, name_len);
    if (!write_block(block, dir_block)) return -EIO;
    dir->size += vol.block_size;
    // A directory we have changed by hand no longer matches its hash index,
    // if it had one: say so, and it is an ordinary directory again
    dir->flags &= ~(uint32_t)INODE_FLAG_INDEX;
    return 0;
}

// Find the entry called `name`. With `remove`, take it out; with `new_ino`
// non-zero, make it name that inode instead (used for ".."). Returns the
// inode number it had, or 0 if there is no such entry.
static uint32_t dir_change(struct ext2_inode *dir, const char *name, bool remove, uint32_t new_ino) {
    size_t name_len = strlen(name);
    uint32_t blocks = dir->size / vol.block_size;
    bool error = false;

    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t block = file_block(dir, b, false, 0, &error);
        if (error) return 0;
        if (block == 0 || !read_block(block, dir_block)) continue;

        uint32_t previous = vol.block_size; // No record before the first
        for (uint32_t offset = 0; offset < vol.block_size; ) {
            if (!record_ok(dir_block, offset)) break;
            uint32_t length = get16(dir_block + offset + 4);
            uint32_t ino = get32(dir_block + offset);

            if (ino != 0 && dir_block[offset + 6] == name_len && memcmp(dir_block + offset + 8, name, name_len) == 0) {
                if (remove) {
                    if (previous < vol.block_size) {
                        // Its space goes to the record before it
                        put16(dir_block + previous + 4, (uint16_t)(get16(dir_block + previous + 4) + length));
                    } else {
                        put32(dir_block + offset, 0); // First in its block: mark it unused
                    }
                } else if (new_ino) {
                    put32(dir_block + offset, new_ino);
                }
                if ((remove || new_ino) && !write_block(block, dir_block)) return 0;
                return ino;
            }
            previous = offset;
            offset += length;
        }
    }
    return 0;
}

// --- What the VFS sees -----------------------------------------------------------

static uint32_t ino_of(const vnode_t *node) {
    const ext2_node_t *info = (const ext2_node_t *)node->private_data;
    return info ? info->ino : 0;
}

// Make a vnode show what an inode says
static void apply_inode(vnode_t *node, const struct ext2_inode *inode) {
    node->uid = inode->uid | ((uint32_t)inode->uid_high << 16);
    node->gid = inode->gid | ((uint32_t)inode->gid_high << 16);
    node->mode = inode->mode & 07777 & 04777;       // The bits this kernel knows
    node->size = (node->type == VFS_FILE) ? inode->size : 0;
    node->mtime = unix_to_packed(inode->mtime);
}

static vnode_t *attach(vnode_t *dir, const char *name, uint32_t ino, const struct ext2_inode *inode) {
    bool directory = (inode->mode & MODE_TYPE_MASK) == MODE_DIRECTORY;
    ext2_node_t *info = kmalloc(sizeof(ext2_node_t));
    if (!info) return NULL;
    info->ino = ino;

    vnode_t *node = vfs_attach(dir, name, directory ? VFS_DIRECTORY : VFS_FILE);
    if (!node) {
        kfree(info);
        return NULL;
    }
    node->ops = directory ? &ext2_dir_ops : &ext2_file_ops;
    node->private_data = (uint64_t)info;
    node->unloaded = directory; // Read when someone first looks inside
    apply_inode(node, inode);
    return node;
}

// Read a directory's entries and attach a node for each. Called by the VFS
// the first time something looks inside the directory.
static int ext2_dir_load(vnode_t *dir) {
    uint32_t dir_ino = ino_of(dir);
    struct ext2_inode inode, child;
    int result = 0;

    mutex_lock(&ext2_mutex);
    if (!read_inode(dir_ino, &inode) || (inode.mode & MODE_TYPE_MASK) != MODE_DIRECTORY) {
        mutex_unlock(&ext2_mutex);
        return -EIO;
    }

    // The entries are collected first and the inodes read afterwards: both
    // use scratch blocks, and a directory block must not change under us
    uint32_t blocks = inode.size / vol.block_size;
    if (blocks > 1024) blocks = 1024; // 65536 entries and more: enough
    uint8_t *copy = kmalloc(vol.block_size);
    bool error = false;

    for (uint32_t b = 0; copy && b < blocks; b++) {
        uint32_t block = file_block(&inode, b, false, 0, &error);
        if (error) { result = -EIO; break; }
        if (block == 0 || !read_block(block, copy)) continue;

        for (uint32_t offset = 0; offset < vol.block_size; ) {
            if (!record_ok(copy, offset)) break;
            uint32_t length = get16(copy + offset + 4);
            uint32_t ino = get32(copy + offset);
            uint32_t name_len = copy[offset + 6];
            const char *raw = (const char *)copy + offset + 8;
            offset += length;

            if (ino == 0 || !valid_inode(ino) || name_len == 0 || name_len > NAME_MAX_LEN) continue;
            if (raw[0] == '.' && (name_len == 1 || (name_len == 2 && raw[1] == '.'))) continue; // "." and ".."

            char name[NAME_MAX_LEN + 1];
            bool printable = true;
            for (uint32_t i = 0; i < name_len; i++) {
                name[i] = raw[i];
                if (raw[i] == '/' || raw[i] == '\0') printable = false;
            }
            name[name_len] = '\0';
            if (!printable || vfs_find_child(dir, name)) continue; // Not a possible name, or there twice

            // Only files and directories: no links, devices or sockets
            if (!read_inode(ino, &child)) continue;
            uint16_t type = child.mode & MODE_TYPE_MASK;
            if (type != MODE_REGULAR && type != MODE_DIRECTORY) continue;
            if (type == MODE_REGULAR && child.size_high != 0) continue; // 4 GiB and more
            attach(dir, name, ino, &child);
        }
    }
    if (!copy) result = -ENOMEM;
    kfree(copy);
    mutex_unlock(&ext2_mutex);
    return result;
}

static bool valid_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX_LEN) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '/' || (unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7E) return false;
    }
    return true;
}

static int ext2_dir_create(vnode_t *dir, const char *name, bool directory, vnode_t **out) {
    if (!valid_name(name)) return -EINVAL;
    uint32_t dir_ino = ino_of(dir);
    struct ext2_inode parent, inode;
    int err = 0;
    uint32_t ino = 0;

    mutex_lock(&ext2_mutex);
    if (!read_inode(dir_ino, &parent)) err = -EIO;
    if (err == 0 && directory && parent.links >= 65000) err = -EMFILE; // ".." of every subdirectory is a link to it

    // The inode first, complete, and only then the entry that names it: a
    // crash in between loses the new file, not the directory
    if (err == 0) {
        ino = alloc_inode(group_of_inode(dir_ino), directory);
        if (!ino) err = -ENOSPC;
    }
    if (err == 0) {
        uint32_t now = unix_now();
        memset(&inode, 0, sizeof(inode));
        // Owner, group and permission bits are set by the caller straight
        // after (ext2_setattr): root's and private until then
        inode.mode = (uint16_t)((directory ? MODE_DIRECTORY : MODE_REGULAR) | 0700);
        inode.atime = inode.ctime = inode.mtime = now;
        inode.links = directory ? 2 : 1; // A directory is also named by its own "."

        if (directory) {
            bool error = false;
            uint32_t block = file_block(&inode, 0, true, group_of_inode(ino), &error);
            if (!block) {
                err = -ENOSPC;
            } else {
                // "." in 12 bytes, ".." in the rest of the block
                memset(dir_block, 0, vol.block_size);
                put32(dir_block, ino);
                put16(dir_block + 4, 12);
                dir_block[6] = 1; dir_block[7] = FT_DIRECTORY; dir_block[8] = '.';
                put32(dir_block + 12, dir_ino);
                put16(dir_block + 16, (uint16_t)(vol.block_size - 12));
                dir_block[18] = 2; dir_block[19] = FT_DIRECTORY; dir_block[20] = '.'; dir_block[21] = '.';
                if (!write_block(block, dir_block)) err = -EIO;
                inode.size = vol.block_size;
            }
        }
        if (err == 0 && !write_inode(ino, &inode)) err = -EIO;
    }
    if (err == 0) {
        err = dir_add(&parent, dir_ino, name, ino, directory ? FT_DIRECTORY : FT_REGULAR);
        if (err == 0) {
            if (directory) parent.links++;
            parent.mtime = parent.ctime = unix_now();
            if (!write_inode(dir_ino, &parent)) err = -EIO;
        }
    }
    if (err != 0 && ino != 0) {
        // Undo: nothing names the inode
        free_contents(&inode);
        free_inode(ino, directory);
    }
    mutex_unlock(&ext2_mutex);
    if (err != 0) return err;

    vnode_t *node = attach(dir, name, ino, &inode);
    if (!node) return -ENOMEM; // (It exists on disk and will be found when the directory is next read.)
    node->unloaded = false;    // New and empty: nothing to read
    *out = node;
    return 0;
}

// The last name of an inode has gone: give back what it occupied
static void release_inode(uint32_t ino, struct ext2_inode *inode, bool directory) {
    free_contents(inode);
    inode->links = 0;
    inode->dtime = unix_now(); // How e2fsck tells a deleted inode from a lost one
    write_inode(ino, inode);
    free_inode(ino, directory);
}

static int ext2_dir_remove(vnode_t *dir, vnode_t *child) {
    uint32_t dir_ino = ino_of(dir), ino = ino_of(child);
    bool directory = child->type == VFS_DIRECTORY;
    struct ext2_inode parent, inode;
    int err = 0;

    mutex_lock(&ext2_mutex);
    if (!read_inode(dir_ino, &parent) || !read_inode(ino, &inode)) err = -EIO;

    // The name first, then the inode: a crash in between leaves an inode
    // nothing names (which e2fsck finds), not a name for a freed inode
    if (err == 0 && dir_change(&parent, child->name, true, 0) != ino) err = -EIO;
    if (err == 0) {
        if (directory && parent.links > 0) parent.links--; // Its ".." is gone
        parent.mtime = parent.ctime = unix_now();
        write_inode(dir_ino, &parent);

        // A file may have other names (hard links made elsewhere); a
        // directory has only this one and its own "."
        if (directory || inode.links <= 1) {
            release_inode(ino, &inode, directory);
        } else {
            inode.links--;
            write_inode(ino, &inode);
        }
    }
    mutex_unlock(&ext2_mutex);

    if (err == 0) {
        kfree((void *)child->private_data);
        child->private_data = 0;
    }
    return err;
}

// A new name for a file or directory, in the same directory or another. The
// new entry is written before the old one is removed, with the link count
// raised in between, so that at no moment does the inode have fewer names
// than its count says.
static int ext2_dir_rename(vnode_t *old_dir, vnode_t *node, vnode_t *new_dir, const char *new_name) {
    if (!valid_name(new_name)) return -EINVAL;
    uint32_t old_ino = ino_of(old_dir), new_ino = ino_of(new_dir), ino = ino_of(node);
    bool directory = node->type == VFS_DIRECTORY;
    bool same_dir = old_dir == new_dir;
    struct ext2_inode from, to, inode;
    int err = 0;

    mutex_lock(&ext2_mutex);
    if (!read_inode(old_ino, &from) || !read_inode(ino, &inode)) err = -EIO;
    if (err == 0 && !same_dir && !read_inode(new_ino, &to)) err = -EIO;
    struct ext2_inode *target = same_dir ? &from : &to;

    if (err == 0) {
        inode.links++;
        if (!write_inode(ino, &inode)) err = -EIO;
    }
    if (err == 0) err = dir_add(target, new_ino, new_name, ino, directory ? FT_DIRECTORY : FT_REGULAR);
    if (err == 0) {
        dir_change(&from, node->name, true, 0); // (dir_add may have grown `from` if it is also the target: same struct)
        uint32_t now = unix_now();

        if (directory && !same_dir) {
            // Its ".." names the new parent now, and the two parents'
            // link counts follow
            dir_change(&inode, "..", false, new_ino);
            if (from.links > 0) from.links--;
            to.links++;
        }
        from.mtime = from.ctime = now;
        write_inode(old_ino, &from);
        if (!same_dir) {
            to.mtime = to.ctime = now;
            write_inode(new_ino, &to);
        }
    }
    if (inode.links > 1 || err == 0) {
        // Back to the one name it has (whether the new or, on failure, the old)
        if (inode.links > 0) inode.links--;
        inode.ctime = unix_now();
        write_inode(ino, &inode);
    }
    mutex_unlock(&ext2_mutex);
    return err;
}

// Owner, group and permission bits, as the vnode now has them, into the inode
static int ext2_setattr(vnode_t *node) {
    uint32_t ino = ino_of(node);
    struct ext2_inode inode;
    int err = 0;

    mutex_lock(&ext2_mutex);
    if (!read_inode(ino, &inode)) {
        err = -EIO;
    } else {
        inode.uid = (uint16_t)node->uid;
        inode.uid_high = (uint16_t)(node->uid >> 16);
        inode.gid = (uint16_t)node->gid;
        inode.gid_high = (uint16_t)(node->gid >> 16);
        inode.mode = (uint16_t)((inode.mode & MODE_TYPE_MASK) | (node->mode & 07777));
        inode.ctime = unix_now();
        if (!write_inode(ino, &inode)) err = -EIO;
    }
    mutex_unlock(&ext2_mutex);
    return err;
}

static int64_t ext2_file_read(vnode_t *node, size_t offset, void *buf, size_t count) {
    struct ext2_inode inode;
    uint8_t *out = buf;
    size_t done = 0;

    mutex_lock(&ext2_mutex);
    if (!read_inode(ino_of(node), &inode)) {
        mutex_unlock(&ext2_mutex);
        return -EIO;
    }
    if (offset < inode.size && count > inode.size - offset) count = inode.size - offset;
    if (offset >= inode.size) count = 0;

    while (done < count) {
        uint32_t index = (uint32_t)((offset + done) / vol.block_size);
        size_t within = (offset + done) % vol.block_size;
        size_t chunk = vol.block_size - within;
        if (chunk > count - done) chunk = count - done;

        bool error = false;
        uint32_t block = file_block(&inode, index, false, 0, &error);
        if (error) break;
        if (block == 0) {
            memset(out + done, 0, chunk); // A hole: never written, reads as zeros
        } else {
            if (!read_block(block, data_block)) break;
            memcpy(out + done, data_block + within, chunk);
        }
        done += chunk;
    }
    mutex_unlock(&ext2_mutex);
    return (done == 0 && count > 0) ? -EIO : (int64_t)done;
}

static int64_t ext2_file_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    uint32_t ino = ino_of(node);
    struct ext2_inode inode;
    const uint8_t *in = buf;
    size_t done = 0;
    if (offset + count < offset || offset + count > 0x7FFFFFFFu) return -EFBIG;

    mutex_lock(&ext2_mutex);
    if (!read_inode(ino, &inode)) {
        mutex_unlock(&ext2_mutex);
        return -EIO;
    }

    while (done < count) {
        uint32_t index = (uint32_t)((offset + done) / vol.block_size);
        size_t within = (offset + done) % vol.block_size;
        size_t chunk = vol.block_size - within;
        if (chunk > count - done) chunk = count - done;

        bool error = false;
        uint32_t block = file_block(&inode, index, true, group_of_inode(ino), &error);
        if (!block) break; // Full, or beyond what we can address

        // Part of a block: keep the rest of it as it is
        if (chunk < vol.block_size && !read_block(block, data_block)) break;
        memcpy(data_block + within, in + done, chunk);
        if (!write_block(block, data_block)) break;
        done += chunk;
    }

    // The inode last: new blocks reachable only once their contents are there
    if (offset + done > inode.size && done > 0) inode.size = (uint32_t)(offset + done);
    inode.mtime = inode.ctime = unix_now();
    bool saved = write_inode(ino, &inode);
    if (saved) {
        node->size = inode.size;
        node->mtime = unix_to_packed(inode.mtime);
    }
    mutex_unlock(&ext2_mutex);

    if (!saved) return -EIO;
    if (done == 0 && count > 0) return -ENOSPC;
    return (int64_t)done;
}

static int ext2_file_truncate(vnode_t *node) {
    uint32_t ino = ino_of(node);
    struct ext2_inode inode;
    int err = 0;

    mutex_lock(&ext2_mutex);
    if (!read_inode(ino, &inode)) {
        err = -EIO;
    } else {
        // The inode is written with no blocks before the blocks are freed:
        // a crash in between leaks them, rather than leaving the file with
        // blocks that are marked free
        struct ext2_inode old = inode;
        memset(inode.block, 0, sizeof(inode.block));
        inode.size = 0;
        inode.sectors = 0;
        inode.mtime = inode.ctime = unix_now();
        if (!write_inode(ino, &inode)) err = -EIO;
        else free_contents(&old);
        if (err == 0) {
            node->size = 0;
            node->mtime = unix_to_packed(inode.mtime);
        }
    }
    mutex_unlock(&ext2_mutex);
    return err;
}

static const vnode_ops_t ext2_file_ops = { .read = ext2_file_read, .write = ext2_file_write,
                                           .truncate = ext2_file_truncate, .setattr = ext2_setattr };
static const vnode_ops_t ext2_dir_ops = { .create = ext2_dir_create, .remove = ext2_dir_remove,
                                          .rename = ext2_dir_rename, .load = ext2_dir_load, .setattr = ext2_setattr };

// --- Putting right what a crash left behind -----------------------------------------
//
// Writes are ordered so that a crash never leaves a name pointing at
// nothing. What it can leave is the opposite: a block or an inode marked as
// in use that nothing refers to (allocated, and the power went before it was
// linked in), a link count one too high (a rename half done), and free
// counts that no longer add up. None of that harms what is stored, but all
// of it is space lost until someone looks.
//
// So at every mount the whole tree is walked from the root, noting every
// inode and block that is reachable, and the bitmaps and counts are made to
// agree with that. It is e2fsck in miniature, for exactly the damage this
// driver's own interrupted writes can cause. Damage of other kinds (two
// files claiming one block, a directory that is not one) is reported and
// left alone.

static uint8_t *reach_blocks;     // One bit per block: something reachable uses it
static uint8_t *reach_inodes;     // One bit per inode: reachable from the root
static uint16_t *reach_links;     // Per inode: directory entries found that name it
static uint32_t check_conflicts;  // Things found that this check does not repair

static bool bit_test(const uint8_t *bits, uint32_t n) { return (bits[n / 8] >> (n % 8)) & 1; }
static void bit_set(uint8_t *bits, uint32_t n) { bits[n / 8] |= (uint8_t)(1u << (n % 8)); }

static void reach_block(uint32_t block) {
    if (!valid_block(block) || bit_test(reach_blocks, block)) {
        check_conflicts++;        // Outside the volume, or already someone else's
        return;
    }
    bit_set(reach_blocks, block);
}

// While one file's blocks are walked: how many it has (tables of block
// numbers included), and the position in the file of its last data block
static uint32_t walk_blocks;
static int64_t walk_last_index;

// Note a block and, if it is a block of block numbers (`depth` levels above
// the data), everything below it. `index` is the position in the file of
// the first data block it leads to.
static void reach_tree(uint32_t block, int depth, uint64_t index) {
    if (block == 0) return;
    reach_block(block);
    walk_blocks++;
    if (depth == 0) {
        if ((int64_t)index > walk_last_index) walk_last_index = (int64_t)index;
        return;
    }
    if (!valid_block(block)) return;

    uint64_t span = 1; // Data blocks under each entry of this table
    for (int i = 1; i < depth; i++) span *= vol.pointers_per_block;
    uint32_t *numbers = kmalloc(vol.block_size);
    if (numbers && read_block(block, numbers)) {
        for (uint32_t i = 0; i < vol.pointers_per_block; i++) {
            if (numbers[i]) reach_tree(numbers[i], depth - 1, index + i * span);
        }
    }
    kfree(numbers);
}

// Does group `g` hold a copy of the superblock and group descriptors? With
// the "sparse" feature only groups 0, 1 and powers of 3, 5 and 7 do.
static bool group_has_backup(uint32_t g) {
    if (!(get32(superblock + 100) & RO_COMPAT_SPARSE)) return true;
    if (g <= 1) return true;
    for (uint32_t base = 3; base <= 7; base += 2) {
        uint32_t power = base;
        while (power < g) power *= base;
        if (power == g) return true;
    }
    return false;
}

int ext2_check(ext2_check_t *report) {
    ext2_check_t result = {0};
    if (!mounted && !groups) return -1;

    mutex_lock(&ext2_mutex);
    size_t block_bytes = (vol.blocks_count + 7) / 8, inode_bytes = (vol.inodes_count + 8) / 8;
    reach_blocks = kmalloc(block_bytes);
    reach_inodes = kmalloc(inode_bytes);
    reach_links = kmalloc((size_t)(vol.inodes_count + 1) * sizeof(uint16_t));
    uint32_t *queue = kmalloc((size_t)vol.inodes_count * sizeof(uint32_t));
    uint8_t *entries = kmalloc(vol.block_size);
    if (!reach_blocks || !reach_inodes || !reach_links || !queue || !entries) {
        kfree(reach_blocks); kfree(reach_inodes); kfree(reach_links); kfree(queue); kfree(entries);
        mutex_unlock(&ext2_mutex);
        return -1; // Not enough memory to look: leave the volume as it is
    }
    memset(reach_blocks, 0, block_bytes);
    memset(reach_inodes, 0, inode_bytes);
    memset(reach_links, 0, (size_t)(vol.inodes_count + 1) * sizeof(uint16_t));
    check_conflicts = 0;

    // What the filesystem itself occupies: everything before the first
    // group, and in each group the copies of the superblock and descriptors
    // (if it has them), the two bitmaps and the inode table
    uint32_t per_block = vol.block_size / sizeof(struct ext2_group);
    uint32_t table_blocks = (vol.groups + per_block - 1) / per_block;
    uint32_t inode_table_blocks = (vol.inodes_per_group * vol.inode_size + vol.block_size - 1) / vol.block_size;
    for (uint32_t b = 0; b < vol.first_data_block; b++) bit_set(reach_blocks, b);
    for (uint32_t g = 0; g < vol.groups; g++) {
        uint32_t start = vol.first_data_block + g * vol.blocks_per_group;
        if (group_has_backup(g)) {
            for (uint32_t b = 0; b <= table_blocks && start + b < vol.blocks_count; b++) bit_set(reach_blocks, start + b);
        }
        bit_set(reach_blocks, groups[g].block_bitmap);
        bit_set(reach_blocks, groups[g].inode_bitmap);
        for (uint32_t b = 0; b < inode_table_blocks; b++) bit_set(reach_blocks, groups[g].inode_table + b);
    }

    // The tree, from the root, one directory at a time
    uint32_t head = 0, tail = 0;
    queue[tail++] = EXT2_ROOT_INO;
    bit_set(reach_inodes, EXT2_ROOT_INO);

    while (head < tail) {
        uint32_t ino = queue[head++];
        struct ext2_inode inode;
        if (!read_inode(ino, &inode)) { check_conflicts++; continue; }
        bool directory = (inode.mode & MODE_TYPE_MASK) == MODE_DIRECTORY;

        // Its blocks. (Short symbolic links keep their text where the block
        // numbers would be: those have no blocks at all.)
        bool regular = (inode.mode & MODE_TYPE_MASK) == MODE_REGULAR;
        if (regular || directory) {
            uint64_t p = vol.pointers_per_block;
            walk_blocks = 0;
            walk_last_index = -1;
            for (int i = 0; i < DIRECT_BLOCKS; i++) reach_tree(inode.block[i], 0, (uint64_t)i);
            reach_tree(inode.block[12], 1, DIRECT_BLOCKS);
            reach_tree(inode.block[13], 2, DIRECT_BLOCKS + p);
            reach_tree(inode.block[14], 3, DIRECT_BLOCKS + p + p * p);

            // A file was growing when the power went: the blocks listed in
            // a table of block numbers are on the disk already, the inode's
            // own record of its size and block count is not. The blocks are
            // the file's (they hold what had been written, or zeros), so the
            // inode is brought up to date rather than the blocks taken away.
            uint32_t sectors = walk_blocks * vol.sectors_per_block;
            uint64_t reaches = (uint64_t)(walk_last_index + 1) * vol.block_size;
            bool too_short = directory ? inode.size != reaches
                                       : walk_last_index >= 0 && inode.size <= (uint64_t)walk_last_index * vol.block_size;
            if (check_conflicts == 0 && (inode.sectors != sectors || (too_short && reaches <= 0xFFFFFFFFu))) {
                inode.sectors = sectors;
                if (too_short && reaches <= 0xFFFFFFFFu) inode.size = (uint32_t)reaches;
                write_inode(ino, &inode);
                result.sizes_corrected++;
            }
        }
        if (!directory) continue;

        // Its entries: each is one name for the inode it gives
        uint32_t blocks = inode.size / vol.block_size;
        for (uint32_t b = 0; b < blocks; b++) {
            bool error = false;
            uint32_t block = file_block(&inode, b, false, 0, &error);
            if (error || block == 0 || !read_block(block, entries)) continue;
            for (uint32_t offset = 0; offset < vol.block_size; ) {
                if (!record_ok(entries, offset)) { check_conflicts++; break; }
                uint32_t length = get16(entries + offset + 4);
                uint32_t child = get32(entries + offset);
                bool dots = entries[offset + 6] <= 2 && entries[offset + 8] == '.' &&
                            (entries[offset + 6] == 1 || entries[offset + 9] == '.');
                offset += length;
                if (child == 0) continue;
                if (!valid_inode(child)) { check_conflicts++; continue; }

                if (reach_links[child] < 0xFFFF) reach_links[child]++;
                if (dots || bit_test(reach_inodes, child)) continue; // "." and "..", or seen already (another name for it)
                bit_set(reach_inodes, child);
                if (tail < vol.inodes_count) queue[tail++] = child; // Files too: their blocks are noted when they come up
            }
        }
    }

    // Inodes: one that is marked as in use and can not be reached was being
    // created or removed when the power went. One whose link count
    // disagrees with the names found was being renamed.
    for (uint32_t g = 0; g < vol.groups; g++) {
        if (!read_block(groups[g].inode_bitmap, bitmap_block)) continue;
        bool changed = false;
        uint16_t free_here = 0, dirs_here = 0;
        for (uint32_t bit = 0; bit < vol.inodes_per_group; bit++) {
            uint32_t ino = g * vol.inodes_per_group + bit + 1;
            if (ino > vol.inodes_count) break;
            bool in_use = (bitmap_block[bit / 8] >> (bit % 8)) & 1;
            bool reserved = ino < vol.first_ino && ino != EXT2_ROOT_INO;
            bool reachable = bit_test(reach_inodes, ino);
            struct ext2_inode inode;

            if (in_use && !reachable && !reserved && check_conflicts == 0) {
                // Unreachable: let it go. (Its blocks are not marked as
                // reachable, so the pass below frees them.)
                if (read_inode(ino, &inode)) {
                    inode.links = 0;
                    inode.dtime = unix_now();
                    write_inode(ino, &inode); // (Uses its own scratch block: the bitmap here is untouched)
                }
                bitmap_block[bit / 8] &= (uint8_t)~(1u << (bit % 8));
                in_use = false;
                changed = true;
                result.inodes_freed++;
            } else if (reachable && !in_use) {
                bitmap_block[bit / 8] |= (uint8_t)(1u << (bit % 8));
                in_use = true;
                changed = true;
                result.inodes_marked++;
            }
            if (reachable && !reserved && read_inode(ino, &inode)) {
                if (inode.links != reach_links[ino] && reach_links[ino] != 0) {
                    inode.links = reach_links[ino];
                    write_inode(ino, &inode);
                    result.links_corrected++;
                }
                if ((inode.mode & MODE_TYPE_MASK) == MODE_DIRECTORY) dirs_here++;
            } else if (reachable && ino == EXT2_ROOT_INO) {
                dirs_here++;
            }
            if (!in_use) free_here++;
        }
        if (changed) write_block(groups[g].inode_bitmap, bitmap_block);
        if (groups[g].free_inodes != free_here || groups[g].used_dirs != dirs_here) result.counts_corrected++;
        groups[g].free_inodes = free_here;
        groups[g].used_dirs = dirs_here;
    }

    // Blocks: the same, and simpler
    for (uint32_t g = 0; g < vol.groups; g++) {
        if (!read_block(groups[g].block_bitmap, bitmap_block)) continue;
        uint32_t first = vol.first_data_block + g * vol.blocks_per_group;
        bool changed = false;
        uint16_t free_here = 0;
        for (uint32_t bit = 0; bit < vol.blocks_per_group && first + bit < vol.blocks_count; bit++) {
            bool in_use = (bitmap_block[bit / 8] >> (bit % 8)) & 1;
            bool reachable = bit_test(reach_blocks, first + bit);
            if (in_use && !reachable && check_conflicts == 0) {
                bitmap_block[bit / 8] &= (uint8_t)~(1u << (bit % 8));
                in_use = false;
                changed = true;
                result.blocks_freed++;
            } else if (reachable && !in_use) {
                bitmap_block[bit / 8] |= (uint8_t)(1u << (bit % 8));
                in_use = true;
                changed = true;
                result.blocks_marked++;
            }
            if (!in_use) free_here++;
        }
        if (changed) write_block(groups[g].block_bitmap, bitmap_block);
        if (groups[g].free_blocks != free_here) result.counts_corrected++;
        groups[g].free_blocks = free_here;
    }

    // The totals, and everything written back
    uint32_t free_blocks = 0, free_inodes = 0;
    for (uint32_t g = 0; g < vol.groups; g++) {
        free_blocks += groups[g].free_blocks;
        free_inodes += groups[g].free_inodes;
    }
    if (free_blocks != vol.free_blocks || free_inodes != vol.free_inodes) result.counts_corrected++;
    vol.free_blocks = free_blocks;
    vol.free_inodes = free_inodes;
    if (result.counts_corrected || result.blocks_freed || result.inodes_freed || result.blocks_marked || result.inodes_marked) {
        for (uint32_t g = 0; g < vol.groups; g += per_block) write_counts(g);
    }
    result.conflicts = check_conflicts;

    kfree(reach_blocks); kfree(reach_inodes); kfree(reach_links); kfree(queue); kfree(entries);
    reach_blocks = NULL; reach_inodes = NULL; reach_links = NULL;
    mutex_unlock(&ext2_mutex);
    if (report) *report = result;
    return 0;
}

// --- Mounting ----------------------------------------------------------------------

bool ext2_parse_superblock(const uint8_t *sb, uint64_t disk_sectors, ext2_layout_t *out) {
    if (!sb || !out || get16(sb + 56) != EXT2_MAGIC) return false;

    uint32_t log_block = get32(sb + 24);
    if (log_block > 2) return false;                     // 1, 2 or 4 KiB blocks
    out->block_size = 1024u << log_block;
    out->inodes_count = get32(sb + 0);
    out->blocks_count = get32(sb + 4);
    out->first_data_block = get32(sb + 20);
    out->blocks_per_group = get32(sb + 32);
    out->inodes_per_group = get32(sb + 40);
    uint32_t revision = get32(sb + 76);
    out->first_ino = revision >= 1 ? get32(sb + 84) : 11;
    out->inode_size = revision >= 1 ? get16(sb + 88) : 128;
    out->free_blocks = get32(sb + 12);
    out->free_inodes = get32(sb + 16);

    // What we would misread, and what we would damage
    if (revision >= 1) {
        if (get32(sb + 96) & ~(uint32_t)INCOMPAT_FILETYPE) return false;
        if (get32(sb + 100) & ~(uint32_t)(RO_COMPAT_SPARSE | RO_COMPAT_LARGE)) return false;
    }

    // Numbers that make no sense, or would make the arithmetic below wrong
    if (out->blocks_count == 0 || out->inodes_count == 0) return false;
    if ((uint64_t)out->blocks_count * (out->block_size / SECTOR) > disk_sectors) return false; // Larger than the disk
    if (out->first_data_block != (out->block_size == 1024 ? 1u : 0u)) return false;
    if (out->blocks_per_group == 0 || out->blocks_per_group > out->block_size * 8) return false; // One bitmap block per group
    if (out->inodes_per_group == 0 || out->inodes_per_group > out->block_size * 8) return false;
    if (out->inode_size < 128 || out->inode_size > out->block_size || (out->inode_size & (out->inode_size - 1))) return false;
    if (out->first_ino < 11 || out->first_ino > out->inodes_count) return false;

    out->groups = (out->blocks_count - out->first_data_block + out->blocks_per_group - 1) / out->blocks_per_group;
    if (out->groups == 0 || out->groups > 4096) return false;
    if ((uint64_t)out->groups * out->inodes_per_group < out->inodes_count) return false;
    return true;
}

int ext2_mount(const char *mount_point, unsigned disk) {
    if (mounted || !mount_point || disk >= virtio_blk_count()) return -1;

    if (bcache_read_dev(disk, 2, superblock, 2) != 0) return -1;
    ext2_layout_t layout;
    if (!ext2_parse_superblock(superblock, virtio_blk_capacity_of(disk), &layout)) return -1;

    vol.disk = disk;
    vol.block_size = layout.block_size;
    vol.sectors_per_block = layout.block_size / SECTOR;
    vol.pointers_per_block = layout.block_size / 4;
    vol.inodes_count = layout.inodes_count;
    vol.blocks_count = layout.blocks_count;
    vol.first_data_block = layout.first_data_block;
    vol.blocks_per_group = layout.blocks_per_group;
    vol.inodes_per_group = layout.inodes_per_group;
    vol.inode_size = layout.inode_size;
    vol.first_ino = layout.first_ino;
    vol.groups = layout.groups;
    vol.free_blocks = layout.free_blocks;
    vol.free_inodes = layout.free_inodes;
    vol.group_table_block = vol.first_data_block + 1;

    // The group descriptors, whole blocks of them, kept in memory
    uint32_t per_block = vol.block_size / sizeof(struct ext2_group);
    uint32_t table_blocks = (vol.groups + per_block - 1) / per_block;
    groups = kmalloc((size_t)table_blocks * vol.block_size);
    if (!groups) return -1;
    bool ok = true;
    for (uint32_t b = 0; ok && b < table_blocks; b++) {
        ok = read_block(vol.group_table_block + b, (uint8_t *)groups + (size_t)b * vol.block_size);
    }
    // Each group's bitmaps and inode table must be inside the volume
    uint32_t table_size = (vol.inodes_per_group * vol.inode_size + vol.block_size - 1) / vol.block_size;
    for (uint32_t g = 0; ok && g < vol.groups; g++) {
        ok = valid_block(groups[g].block_bitmap) && valid_block(groups[g].inode_bitmap) &&
             valid_block(groups[g].inode_table) && groups[g].inode_table + table_size <= vol.blocks_count;
    }

    struct ext2_inode root_inode;
    ok = ok && read_inode(EXT2_ROOT_INO, &root_inode) && (root_inode.mode & MODE_TYPE_MASK) == MODE_DIRECTORY;

    const char *leaf = NULL;
    vnode_t *parent = ok ? vfs_find_parent(mount_point, &leaf) : NULL;
    vnode_t *root = NULL;
    if (parent) vfs_mount_dir(parent, leaf, &root);
    ext2_node_t *info = (root && !root->children && !root->ops) ? kmalloc(sizeof(ext2_node_t)) : NULL;
    if (!info) {
        kprintf("ext2: disk %u holds an ext2 volume this driver can not use\n", disk);
        kfree(groups);
        groups = NULL;
        return -1;
    }

    info->ino = EXT2_ROOT_INO;
    root->ops = &ext2_dir_ops;
    root->private_data = (uint64_t)info;
    root->unloaded = true;
    apply_inode(root, &root_inode);

    memset(zero_block, 0, sizeof(zero_block));
    mounted = true;
    mount_register(mount_point, "ext2", disk == 0 ? "/dev/vda" : "/dev/vdb");

    // The machine has no orderly shutdown, so every start is "after a
    // crash": see that the volume is whole before anyone uses it
    ext2_check_t check;
    if (ext2_check(&check) == 0) {
        if (check.blocks_freed || check.inodes_freed || check.links_corrected || check.blocks_marked ||
            check.inodes_marked || check.sizes_corrected) {
            kprintf("ext2: repaired after an interrupted write: %u blocks and %u inodes freed, %u link counts and "
                    "%u sizes corrected, %u blocks and %u inodes marked in use\n", check.blocks_freed, check.inodes_freed,
                    check.links_corrected, check.sizes_corrected, check.blocks_marked, check.inodes_marked);
        }
        if (check.conflicts) {
            kprintf("ext2: %u inconsistencies this driver does not repair (nothing was freed): run e2fsck\n", check.conflicts);
        }
    }
    kprintf("ext2: mounted at %s: %u blocks of %u bytes (%u free), %u inodes (%u free)\n",
            mount_point, vol.blocks_count, vol.block_size, vol.free_blocks, vol.inodes_count, vol.free_inodes);
    return 0;
}

bool ext2_mounted(void) { return mounted; }
uint32_t ext2_free_blocks(void) { return mounted ? vol.free_blocks : 0; }
uint32_t ext2_free_inodes(void) { return mounted ? vol.free_inodes : 0; }

#ifdef KERNEL_TESTS
// For tests: put the volume in the state an interrupted create and an
// interrupted rename would leave: a block and an inode marked in use that
// nothing refers to, and a file with one link too many.
bool ext2_test_damage(struct vnode *file_with_extra_link) {
    mutex_lock(&ext2_mutex);
    uint32_t block = alloc_block(0);
    uint32_t ino = alloc_inode(0, false);
    struct ext2_inode inode;
    bool ok = block != 0 && ino != 0;
    if (ok) {
        memset(&inode, 0, sizeof(inode));
        inode.mode = MODE_REGULAR | 0644;
        inode.links = 1;
        inode.block[0] = alloc_block(0); // An inode that got as far as having contents
        inode.sectors = vol.sectors_per_block;
        inode.size = 5;
        ok = inode.block[0] != 0 && write_inode(ino, &inode);
    }
    if (ok && file_with_extra_link) {
        ok = read_inode(ino_of(file_with_extra_link), &inode);
        inode.links++;
        ok = ok && write_inode(ino_of(file_with_extra_link), &inode);
    }
    mutex_unlock(&ext2_mutex);
    return ok;
}

// For tests: drop what is known about a directory's contents, so that the
// next look inside reads it from the disk again (see fat32_forget_directory)
bool ext2_forget_directory(vnode_t *dir) {
    if (!dir || dir->ops != &ext2_dir_ops || dir->unloaded) return false;
    vfs_lock();
    for (vnode_t *c = dir->children; c; c = c->next) {
        if (!c->mount_point && (c->open_count != 0 || c->children)) {
            vfs_unlock();
            return false;
        }
    }
    // Mount points stay: they are not the disk's (vfs_mount_dir)
    vnode_t **link = &dir->children;
    while (*link) {
        vnode_t *c = *link;
        if (c->mount_point) {
            link = &c->next;
            continue;
        }
        *link = c->next;
        kfree((void *)c->private_data);
        kfree(c);
    }
    dir->unloaded = true;
    vfs_unlock();
    return true;
}
#endif
