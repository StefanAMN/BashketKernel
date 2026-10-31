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
#include "bcache.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "spinlock.h"
#include "mutex.h"
#include "kerrno.h"
#include "heap.h"
#include "sched.h"
#include "mount.h"
#include "walltime.h"

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
    uint64_t entry_lba;        // Sector holding its (short) directory entry (0 for the root, which has none)
    uint32_t entry_offset;     // Byte offset of the entry in that sector
    uint32_t dir_index;        // The same place as a slot number in its directory
    uint8_t lfn_count;         // Long-name entries directly in front of it
    uint8_t short_name[11];    // Its 8.3 name as stored, to keep aliases unique
} fat_node_t;

static const vnode_ops_t fat_file_ops;
static const vnode_ops_t fat_dir_ops;

static fat32_layout_t volume; // Not "fs": with Intel-syntax assembly that name is read as the FS register
static bool mounted = false;

// One sector of the FAT is kept in memory: following a chain reads the same
// sector many times over.
static mutex_t volume_mutex = MUTEX_INIT("fat-volume"); // See the comment at the top about the lock
static spinlock_t fat_cache_lock = SPINLOCK_RANKED("fat-cache", LOCK_RANK_FS);
static uint32_t alloc_hint = 2;       // Where the search for a free cluster starts
static bool fsinfo_invalidated = false;

static uint8_t fat_cache[SECTOR];
static uint32_t fat_cache_sector = 0; // 0: nothing cached (sector 0 is never part of the FAT)

// Little-endian fields at any alignment: read 16 and 32 bits at `p`
static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
// Read a little-endian 32-bit field at `p`
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

// Is `cluster` a data cluster of this volume (2 .. cluster_count + 1)?
static bool valid_cluster(uint32_t cluster) {
    return cluster >= 2 && cluster < volume.cluster_count + 2;
}

// The first sector of data cluster `cluster` (which must be valid)
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

    uint64_t flags = spinlock_acquire_irqsave(&fat_cache_lock);
    bool cached = (fat_cache_sector == sector);
    if (cached) entry = read32(fat_cache + index * 4);
    spinlock_release_irqrestore(&fat_cache_lock, flags);

    if (!cached) {
        // Read into a private buffer: the disk read sleeps, and another task
        // may use the shared cache meanwhile
        uint8_t fresh[SECTOR];
        if (bcache_read(sector, fresh, 1) != 0) return 0;
        entry = read32(fresh + index * 4);

        flags = spinlock_acquire_irqsave(&fat_cache_lock);
        memcpy(fat_cache, fresh, SECTOR);
        fat_cache_sector = sector;
        spinlock_release_irqrestore(&fat_cache_lock, flags);
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
        if (bcache_read(lba, sector, 1) != 0) return done ? (int64_t)done : -EIO;

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

// Take and release the lock every change to the volume holds (it sleeps)
static void volume_lock(void) {
    mutex_lock(&volume_mutex);
}

// (See volume_lock)
static void volume_unlock(void) {
    mutex_unlock(&volume_mutex);
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
    if (bcache_read(lba, sector, 1) != 0) return;
    if (read32(sector) != 0x41615252 || read32(sector + 484) != 0x61417272) return; // Not an FSInfo sector
    memset(sector + 488, 0xFF, 8); // Free count and next-free hint: unknown
    bcache_write(lba, sector, 1);
}

// Set the FAT entry of `cluster` to `value` (a next cluster, FAT_END_MARK
// or FAT_FREE) in every copy of the FAT, keeping its reserved top bits, and
// in the cached sector. Caller holds the volume lock. False on a disk error.
static bool set_fat_entry(uint32_t cluster, uint32_t value) {
    if (!valid_cluster(cluster)) return false;
    invalidate_fsinfo();

    uint32_t offset_sector = cluster / (SECTOR / 4);
    uint32_t index = cluster % (SECTOR / 4);
    uint8_t sector[SECTOR];

    if (bcache_read(volume.fat_start + offset_sector, sector, 1) != 0) return false;

    // The top four bits of an entry are reserved and must be preserved
    uint32_t entry = (read32(sector + index * 4) & ~FAT_ENTRY_MASK) | (value & FAT_ENTRY_MASK);
    sector[index * 4 + 0] = (uint8_t)entry;
    sector[index * 4 + 1] = (uint8_t)(entry >> 8);
    sector[index * 4 + 2] = (uint8_t)(entry >> 16);
    sector[index * 4 + 3] = (uint8_t)(entry >> 24);

    // Every copy of the FAT gets the same change
    for (uint32_t copy = 0; copy < volume.fat_count; copy++) {
        uint32_t lba = volume.fat_start + copy * volume.fat_sectors + offset_sector;
        if (bcache_write(lba, sector, 1) != 0) return false;
    }

    uint64_t flags = spinlock_acquire_irqsave(&fat_cache_lock);
    if (fat_cache_sector == volume.fat_start + offset_sector) memcpy(fat_cache, sector, SECTOR);
    spinlock_release_irqrestore(&fat_cache_lock, flags);
    return true;
}

// Fill a cluster with zeros on the disk. False on a disk error.
static bool zero_cluster(uint32_t cluster) {
    uint8_t zeros[SECTOR];
    memset(zeros, 0, SECTOR);
    for (uint32_t s = 0; s < volume.sectors_per_cluster; s++) {
        if (bcache_write(cluster_to_sector(cluster) + s, zeros, 1) != 0) return false;
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
            if (bcache_read(lba, sector, 1) != 0) return 0;
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

// Give back every cluster of a chain (stopping after as many steps as the
// volume has clusters: a damaged chain may loop). Caller holds the volume lock.
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

static void fat_now(uint16_t *date, uint16_t *time);
static void fat_from_unix(uint32_t seconds, uint16_t *date, uint16_t *time);

// Change the first cluster and size recorded in a node's directory entry,
// and stamp it as written now. Caller holds the volume lock.
static bool update_entry(const fat_node_t *info, uint32_t first_cluster, uint32_t size) {
    if (info->entry_lba == 0) return true; // The root directory has no entry

    uint8_t sector[SECTOR];
    if (bcache_read(info->entry_lba, sector, 1) != 0) return false;

    struct dir_entry *entry = (struct dir_entry *)(sector + info->entry_offset);
    entry->cluster_high = (uint16_t)(first_cluster >> 16);
    entry->cluster_low = (uint16_t)first_cluster;
    entry->size = size;
    uint16_t date, time;
    fat_now(&date, &time);                           // The contents have just changed
    entry->write_date = entry->access_date = date;
    entry->write_time = time;
    return bcache_write(info->entry_lba, sector, 1) == 0;
}

// --- Names --------------------------------------------------------------------
//
// Every file has a short name: eight characters and three of extension, upper
// case, stored in its 32-byte directory entry. A name that does not fit that
// (too long, mixed case, a space, two dots) is stored as well, in "long file
// name" entries placed directly before the short one: 13 UTF-16 characters
// each, the last part first, each carrying a checksum of the short name so
// that software which knows nothing of long names, and deletes or renames
// the short entry, leaves long entries behind that can be recognised as
// orphans. The short name is then an invented alias ("A LONG~1.TXT").

#define LFN_CHARS 13                 // Characters per long-name entry
#define LFN_LAST  0x40               // Flag in the ordinal of the final part (which comes first on disk)
#define NAME_MAX_LEN 127             // A vnode's name holds this many

// The checksum of a short name that its long-name entries carry
// (Microsoft's FAT specification, "Long Directory Entries")
static uint8_t lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

// ASCII upper case of `c`
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// FAT names keep their case but are compared without it
static bool names_equal_nocase(const char *a, const char *b) {
    while (*a && *b && upper(*a) == upper(*b)) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

// May this be a (long) file name? Printable ASCII without the characters
// that mean something in a path or to DOS, not ending in a space or a dot.
static bool valid_long_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX_LEN) return false;
    if (name[0] == ' ' || name[len - 1] == ' ' || name[len - 1] == '.') return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c < 0x20 || c > 0x7E || strchr("\"*/:<>?\\|", c)) return false;
    }
    return true;
}

// "notes.txt" -> "NOTES   TXT" plus the lower-case flags. FAT short names are
// upper case; a name that is all lower case is stored upper case with a flag
// saying "show it in lower case". Returns false for a name that does not fit
// a short entry exactly: it then needs long-name entries.
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
        bool is_upper = (c >= 'A' && c <= 'Z');
        if (!lower && !is_upper && !strchr(allowed, c)) return false;
        if (lower) c = (char)(c - 32);

        if (in_ext) {
            if (ext_len >= 3) return false;
            out[8 + ext_len++] = (uint8_t)c;
            ext_lower |= lower;
            ext_upper |= is_upper;
        } else {
            if (base_len >= 8) return false;
            out[base_len++] = (uint8_t)c;
            base_lower |= lower;
            base_upper |= is_upper;
        }
    }

    if (base_len == 0 || (in_ext && ext_len == 0)) return false;
    if ((base_lower && base_upper) || (ext_lower && ext_upper)) return false; // Mixed case
    *nt_flags = (uint8_t)((base_lower ? NT_LOWER_BASE : 0) | (ext_lower ? NT_LOWER_EXT : 0));
    return true;
}

// The short alias number `n` for a long name: up to six characters of the
// name, "~n", and up to three of what follows its last dot
static void make_alias(const char *name, unsigned n, uint8_t out[11]) {
    static const char allowed[] = "0123456789_-!#$%&'()@^`{}";
    char tail[8];
    size_t tail_len = 0;
    tail[tail_len++] = '~';
    char digits[6];
    int count = 0;
    do { digits[count++] = (char)('0' + n % 10); n /= 10; } while (n && count < 6);
    while (count > 0) tail[tail_len++] = digits[--count];

    const char *dot = NULL;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (dot == name) dot = NULL; // A leading dot starts the name; it does not introduce an extension

    memset(out, ' ', 11);
    size_t base_len = 0, base_max = 8 - tail_len;
    for (const char *p = name; *p && p != dot && base_len < base_max; p++) {
        char c = upper(*p);
        if ((c >= 'A' && c <= 'Z') || strchr(allowed, c)) out[base_len++] = (uint8_t)c;
    }
    if (base_len == 0) out[base_len++] = '_';
    memcpy(out + base_len, tail, tail_len);

    size_t ext_len = 0;
    for (const char *p = dot ? dot + 1 : ""; *p && ext_len < 3; p++) {
        char c = upper(*p);
        if ((c >= 'A' && c <= 'Z') || strchr(allowed, c)) out[8 + ext_len++] = (uint8_t)c;
    }
}

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

// Where the 13 characters of a long-name entry are kept: around the fields
// an old program expects to find in a 32-byte entry
static const uint8_t lfn_char_offset[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

// Build long-name entry number `ordinal` (1 is the first 13 characters) of
// `total` for `name`
static void fill_lfn_entry(uint8_t entry[32], const char *name, unsigned ordinal, unsigned total, uint8_t checksum) {
    size_t len = strlen(name);
    memset(entry, 0, 32);
    entry[0] = (uint8_t)(ordinal | (ordinal == total ? LFN_LAST : 0));
    entry[11] = ATTR_LONG_NAME;
    entry[13] = checksum;
    for (unsigned k = 0; k < LFN_CHARS; k++) {
        size_t at = (size_t)(ordinal - 1) * LFN_CHARS + k;
        // The name, a zero after it if there is room, then 0xFFFF padding
        uint16_t unit = at < len ? (uint8_t)name[at] : at == len ? 0x0000 : 0xFFFF;
        entry[lfn_char_offset[k]] = (uint8_t)unit;
        entry[lfn_char_offset[k] + 1] = (uint8_t)(unit >> 8);
    }
}

// --- Dates --------------------------------------------------------------------

// A Unix time as FAT packs a date and time: years since 1980 in 7 bits,
// month in 4, day in 5; hours in 5, minutes in 6, seconds/2 in 5
static void fat_from_unix(uint32_t seconds, uint16_t *date, uint16_t *time) {
    struct rtc_time t;
    walltime_to_date(seconds, &t);
    unsigned year = t.year >= 1980 && t.year < 2108 ? t.year - 1980u : 0;
    *date = (uint16_t)((year << 9) | ((unsigned)t.month << 5) | t.day);
    *time = (uint16_t)(((unsigned)t.hour << 11) | ((unsigned)t.minute << 5) | (t.second / 2));
}

// The date and time now, as FAT packs them
static void fat_now(uint16_t *date, uint16_t *time) {
    fat_from_unix(walltime_seconds(), date, time);
}

// A FAT date and time as seconds since 1970 (what the VFS keeps), or 0
// for a date that is not one (an entry written without a date)
static uint32_t fat_to_unix(uint16_t date, uint16_t time) {
    struct rtc_time t = {
        .year = (uint16_t)(1980 + (date >> 9)), .month = (uint8_t)((date >> 5) & 15), .day = (uint8_t)(date & 31),
        .hour = (uint8_t)(time >> 11), .minute = (uint8_t)((time >> 5) & 63), .second = (uint8_t)((time & 31) * 2),
    };
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.hour > 23 || t.minute > 59 || t.second > 59) return 0;
    return (uint32_t)walltime_from_date(&t);
}

// A fresh short directory entry: name, case flags, attributes, first
// cluster, size 0, and the creation and write times set to now
static void fill_entry(struct dir_entry *entry, const uint8_t name[11], uint8_t nt_flags, uint8_t attr, uint32_t cluster) {
    uint16_t date, time;
    fat_now(&date, &time);
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, name, 11);
    entry->attr = attr;
    entry->nt_flags = nt_flags;
    entry->create_date = entry->access_date = entry->write_date = date;
    entry->create_time = entry->write_time = time;
    entry->cluster_high = (uint16_t)(cluster >> 16);
    entry->cluster_low = (uint16_t)cluster;
}

// --- Directory slots -----------------------------------------------------------
//
// A directory is an array of 32-byte slots spread over its cluster chain.
// Slots are named here by their index in that array.

#define SLOTS_PER_SECTOR (SECTOR / sizeof(struct dir_entry))

// Where slot `index` of the directory starting at `first_cluster` is on disk
static bool slot_location(uint32_t first_cluster, uint32_t index, uint64_t *lba, uint32_t *offset) {
    uint32_t per_cluster = volume.sectors_per_cluster * SLOTS_PER_SECTOR;
    uint32_t cluster = first_cluster;
    for (uint32_t skip = index / per_cluster; skip > 0; skip--) {
        cluster = next_cluster(cluster);
        if (!cluster) return false; // Past the end of the directory
    }
    if (!valid_cluster(cluster)) return false;
    uint32_t in_cluster = index % per_cluster;
    *lba = cluster_to_sector(cluster) + in_cluster / SLOTS_PER_SECTOR;
    *offset = (uint32_t)((in_cluster % SLOTS_PER_SECTOR) * sizeof(struct dir_entry));
    return true;
}

// Write the 32 bytes of `entry` into slot `index` of the directory starting
// at `first_cluster`. False if the slot is past its end or on a disk error.
static bool write_slot(uint32_t first_cluster, uint32_t index, const uint8_t entry[32]) {
    uint8_t sector[SECTOR];
    uint64_t lba;
    uint32_t offset;
    if (!slot_location(first_cluster, index, &lba, &offset)) return false;
    if (bcache_read(lba, sector, 1) != 0) return false;
    memcpy(sector + offset, entry, 32);
    return bcache_write(lba, sector, 1) == 0;
}

// Mark `count` slots from `first_index` as deleted
static bool delete_slots(uint32_t first_cluster, uint32_t first_index, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint8_t sector[SECTOR];
        uint64_t lba;
        uint32_t offset;
        if (!slot_location(first_cluster, first_index + i, &lba, &offset)) return false;
        if (bcache_read(lba, sector, 1) != 0) return false;
        sector[offset] = 0xE5;
        if (bcache_write(lba, sector, 1) != 0) return false;
    }
    return true;
}

// Find `count` unused slots in a row (a name's long entries and its short
// entry must be together), extending the directory by a cluster at a time
// if it has no such run. Returns the index of the first.
static bool find_free_run(const fat_node_t *dir, uint32_t count, uint32_t *first_index) {
    uint32_t per_cluster = volume.sectors_per_cluster * SLOTS_PER_SECTOR;
    uint32_t cluster = dir->first_cluster, last = 0;
    uint32_t index = 0, run_start = 0, run = 0;
    uint8_t sector[SECTOR];

    for (uint32_t steps = 0; valid_cluster(cluster) && steps <= volume.cluster_count && index < MAX_DIR_ENTRIES; steps++) {
        for (uint32_t s = 0; s < volume.sectors_per_cluster; s++) {
            if (bcache_read(cluster_to_sector(cluster) + s, sector, 1) != 0) return false;
            for (uint32_t offset = 0; offset < SECTOR; offset += sizeof(struct dir_entry), index++) {
                // 0x00: never used (and nothing follows); 0xE5: deleted
                if (sector[offset] == 0x00 || sector[offset] == 0xE5) {
                    if (run == 0) run_start = index;
                    if (++run == count) {
                        *first_index = run_start;
                        return true;
                    }
                } else {
                    run = 0;
                }
            }
        }
        last = cluster;
        cluster = next_cluster(cluster);
    }
    if (!last || index + count > MAX_DIR_ENTRIES) return false;

    // No such run: the free slots at the end (if any) plus new clusters
    if (run == 0) run_start = index;
    while (run < count) {
        uint32_t added = allocate_cluster();
        if (!added) return false;
        if (!zero_cluster(added) || !set_fat_entry(last, added)) {
            set_fat_entry(added, FAT_FREE);
            return false;
        }
        last = added;
        run += per_cluster;
    }
    *first_index = run_start;
    return true;
}

// --- File operations ----------------------------------------------------------

// vnode_ops_t.read: up to `count` bytes from `offset` (takes no lock: see
// the top of this file). Returns the bytes read or -EIO.
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
            if (chunk < SECTOR && bcache_read(lba, sector, 1) != 0) { failed = true; break; } // Keep the rest of the sector
            memcpy(sector + in_sector, data + done, chunk);
            if (bcache_write(lba, sector, 1) != 0) { failed = true; break; }

            done += chunk;
            from += chunk;
        }
        if (failed) break;

        previous = cluster;
        cluster = next_cluster(cluster);
    }

    // Record the new size (and first cluster) in the directory entry last
    size_t reached = (done > 0) ? offset + done : 0;
    // The entry is rewritten after every write that wrote anything, even
    // if neither size nor first cluster changed: it also records when the
    // file was last written
    if (done > 0 || first_cluster_changed) {
        size_t new_size = reached > node->size ? reached : node->size;
        if (!update_entry(info, info->first_cluster, (uint32_t)new_size)) return -EIO;
        node->size = new_size;
        node->mtime = walltime_seconds();
    }

    if (done == 0 && count > 0) return -ENOSPC;
    return (int64_t)done;
}

// vnode_ops_t.write: `count` bytes at `offset` (at most MAX_WRITE_BYTES per
// call: a short write, which the caller continues). `buf` is kernel memory
// (system calls copy user data through a page of their own), so nothing
// done under the volume lock can fault. Returns the bytes written, -ENOSPC
// when the volume is full or the file would pass 4 GiB, or -EIO.
static int64_t fat_file_write(vnode_t *node, size_t offset, const void *buf, size_t count) {
    if (count == 0) return 0;
    if (count > MAX_WRITE_BYTES) count = MAX_WRITE_BYTES;
    if (offset > 0xFFFFFFFFu || count > 0xFFFFFFFFu - offset) return -ENOSPC; // FAT32 files end at 4 GiB

    volume_lock();
    int64_t result = write_locked(node, offset, buf, count);
    volume_unlock();
    return result;
}

// vnode_ops_t.truncate: cut the file to `size` bytes. The entry first
// (new size, and no first cluster for an empty file), then the chain is
// cut after the last cluster kept and the rest freed: a crash in between
// leaves lost clusters, never an entry pointing at freed ones. The rest of
// the last cluster is zeroed. Returns 0 or -EIO.
static int fat_file_truncate(vnode_t *node, size_t size) {
    fat_node_t *info = (fat_node_t *)node->private_data;
    if (size > 0) {
        size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR;
        volume_lock();
        uint32_t last = info->first_cluster;                  // The cluster holding byte size - 1
        for (size_t at = cluster_bytes; at < size && valid_cluster(last); at += cluster_bytes) last = next_cluster(last);
        bool ok = valid_cluster(last);
        // Zero the rest of the last cluster, from byte `size` on
        size_t within = size % cluster_bytes;
        for (size_t s = within / SECTOR; ok && within && s < volume.sectors_per_cluster; s++) {
            uint8_t sector[SECTOR];
            uint64_t lba = cluster_to_sector(last) + s;
            size_t from = (s == within / SECTOR) ? within % SECTOR : 0;
            if (from && bcache_read(lba, sector, 1) != 0) { ok = false; break; }
            memset(sector + from, 0, SECTOR - from);
            ok = bcache_write(lba, sector, 1) == 0;
        }
        uint32_t rest = ok ? next_cluster(last) : 0;
        ok = ok && update_entry(info, info->first_cluster, (uint32_t)size);
        if (ok && valid_cluster(rest)) {
            ok = set_fat_entry(last, FAT_END_MARK);
            if (ok) free_chain(rest);
        }
        if (ok) {
            node->size = size;
            node->mtime = walltime_seconds();
        }
        volume_unlock();
        return ok ? 0 : -EIO;
    }

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

// --- Reading directories ---------------------------------------------------------

static bool name_taken_nocase(const vnode_t *dir, const char *name, const vnode_t *except);

// Attach a node for one directory entry. Returns NULL if memory ran out.
static vnode_t *attach_entry(vnode_t *dir, const char *name, const struct dir_entry *entry,
                             uint64_t entry_lba, uint32_t entry_offset, uint32_t dir_index, unsigned lfn_count) {
    if (name_taken_nocase(dir, name, NULL)) return NULL; // Two entries with one name: keep the first
    bool directory = (entry->attr & ATTR_DIRECTORY) != 0;
    uint32_t first = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;

    fat_node_t *info = kmalloc(sizeof(fat_node_t));
    if (!info) return NULL;
    memset(info, 0, sizeof(*info));
    info->first_cluster = (directory || entry->size) ? first : 0; // An empty file has no cluster
    info->entry_lba = entry_lba;
    info->entry_offset = entry_offset;
    info->dir_index = dir_index;
    info->lfn_count = (uint8_t)lfn_count;
    memcpy(info->short_name, entry->name, 11);

    vnode_t *node = vfs_attach(dir, name, directory ? VFS_DIRECTORY : VFS_FILE);
    if (!node) {
        kfree(info);
        return NULL;
    }
    // FAT has nowhere to keep an owner or permission bits. Everything on the
    // volume belongs to root; everyone may read it and run it.
    node->mode = 0755;
    node->ops = directory ? &fat_dir_ops : &fat_file_ops;
    node->private_data = (uint64_t)info;
    node->size = directory ? 0 : entry->size;
    node->mtime = fat_to_unix(entry->write_date, entry->write_time);
    // A directory's own entries are read when someone first looks inside
    node->unloaded = directory;
    return node;
}

// How many directories of this volume are above `dir`
static int depth_of(const vnode_t *dir) {
    int depth = 0;
    for (const vnode_t *d = dir; d && d->ops == &fat_dir_ops && depth <= MAX_DEPTH; d = d->parent) depth++;
    return depth;
}

// vnode_ops_t.load: read a directory's entries from the disk and attach a
// node for each: the VFS calls this, holding the tree lock, the first time
// something looks inside the directory. Long names are put together from
// the entries in front of each short one. Returns 0 or -EIO.
static int fat_dir_load(vnode_t *dir) {
    const fat_node_t *dir_info = (const fat_node_t *)dir->private_data;
    if (!dir_info) return -EIO;
    // A damaged volume can have a directory that contains itself: each
    // level would load the next for as long as someone keeps descending
    if (depth_of(dir) > MAX_DEPTH) return 0;

    uint32_t cluster = dir_info->first_cluster;
    uint32_t index = 0;

    // The long name being collected: its parts arrive last first
    char long_name[NAME_MAX_LEN + LFN_CHARS + 1];
    unsigned expected = 0, total = 0;   // Next ordinal wanted (0: none in progress), and the number of parts
    uint8_t checksum = 0;
    bool usable = false;                // Every character so far is one we can show

    for (uint32_t steps = 0; valid_cluster(cluster) && steps <= volume.cluster_count; steps++) {
        for (uint32_t s = 0; s < volume.sectors_per_cluster; s++) {
            uint8_t sector[SECTOR];
            uint64_t lba = cluster_to_sector(cluster) + s;
            if (bcache_read(lba, sector, 1) != 0) return -EIO;

            for (uint32_t offset = 0; offset < SECTOR; offset += sizeof(struct dir_entry), index++) {
                const struct dir_entry *entry = (const struct dir_entry *)(sector + offset);
                const uint8_t *raw = sector + offset;

                if (raw[0] == 0x00) return 0;                          // No entries after this one
                if (index >= MAX_DIR_ENTRIES) return 0;
                if (raw[0] == 0xE5) { expected = 0; continue; }        // Deleted

                if ((entry->attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
                    unsigned ordinal = raw[0] & 0x1F;
                    if (raw[0] & LFN_LAST) {
                        // The start of a name (its last 13 characters)
                        total = ordinal;
                        expected = (ordinal >= 1 && (ordinal - 1) * LFN_CHARS < NAME_MAX_LEN + 1) ? ordinal : 0;
                        checksum = raw[13];
                        usable = expected != 0;
                        memset(long_name, 0, sizeof(long_name));
                    } else if (expected == 0 || ordinal != expected || raw[13] != checksum) {
                        expected = 0;                                  // Out of sequence: an orphan
                    }
                    if (expected != 0) {
                        for (unsigned k = 0; k < LFN_CHARS; k++) {
                            uint16_t unit = (uint16_t)(raw[lfn_char_offset[k]] | (raw[lfn_char_offset[k] + 1] << 8));
                            if (unit == 0x0000 || unit == 0xFFFF) continue; // The end of the name, and padding
                            if (unit < 0x20 || unit > 0x7E) usable = false; // Not ASCII: shown by its short name
                            long_name[(expected - 1) * LFN_CHARS + k] = (char)unit;
                        }
                        expected--;                                    // 0 once part 1 has been seen... (see below)
                        if (expected == 0) expected = 0xFF;            // ...marked as "complete, short entry next"
                    }
                    continue;
                }

                // A short entry. It takes the long name collected just before
                // it, if that is complete and was made for this very entry.
                bool has_long = expected == 0xFF && usable && checksum == lfn_checksum(entry->name) &&
                                valid_long_name(long_name);
                unsigned lfn_count = has_long ? total : 0;
                expected = 0;

                if (entry->attr & ATTR_VOLUME_ID) continue;             // The volume's label
                if (entry->name[0] == '.') continue;                    // "." and ".."

                char short_text[13];
                const char *name = long_name;
                if (!has_long) {
                    if (!decode_name(entry, short_text)) continue;
                    name = short_text;
                }

                uint32_t first = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
                if (entry->attr & ATTR_DIRECTORY) {
                    if (!valid_cluster(first)) continue;
                } else if (entry->size > 0 && !valid_cluster(first)) {
                    continue;                                           // Anything but an empty file needs a cluster
                }
                attach_entry(dir, name, entry, lba, offset, index, lfn_count);
            }
        }
        cluster = next_cluster(cluster);
    }
    return 0;
}

// Write the directory entries for `name` into `dir`: long-name entries if it
// needs them, then the short entry, in slots next to each other. The short
// entry is a copy of `like` under the new name if that is given (a rename),
// else a new one with these attributes and first cluster. Fills in where
// the entries went. `except` is a child of `dir` whose short name need not
// be avoided (the file being renamed). Caller holds the volume lock.
static int store_name(vnode_t *dir, const char *name, const struct dir_entry *like, uint8_t attr,
                      uint32_t cluster, fat_node_t *info, const vnode_t *except) {
    const fat_node_t *dir_info = (const fat_node_t *)dir->private_data;
    uint8_t short_name[11], nt_flags = 0;
    unsigned lfn_count = 0;
    bool fits = encode_name(name, short_name, &nt_flags);

    // A long name gets an invented short one; try numbers until one is free.
    // (A name that is short already must not collide with someone's alias.)
    for (unsigned n = 1; ; n++) {
        if (!fits) {
            make_alias(name, n, short_name);
            lfn_count = (unsigned)((strlen(name) + LFN_CHARS - 1) / LFN_CHARS);
        }
        bool taken = false;
        for (const vnode_t *c = dir->children; c && !taken; c = c->next) {
            const fat_node_t *other = (const fat_node_t *)c->private_data;
            taken = c != except && other && memcmp(other->short_name, short_name, 11) == 0;
        }
        if (!taken) break;
        if (fits) return -EEXIST;
        if (n >= 99999) return -ENOSPC;
    }

    uint32_t first;
    if (!find_free_run(dir_info, lfn_count + 1, &first)) return -ENOSPC;

    // On disk the last part of the long name comes first, the short entry last
    uint8_t checksum = lfn_checksum(short_name);
    for (unsigned i = 0; i < lfn_count; i++) {
        uint8_t entry[32];
        fill_lfn_entry(entry, name, lfn_count - i, lfn_count, checksum);
        if (!write_slot(dir_info->first_cluster, first + i, entry)) {
            delete_slots(dir_info->first_cluster, first, i);
            return -EIO;
        }
    }

    struct dir_entry entry;
    if (like) {
        entry = *like;
        memcpy(entry.name, short_name, 11);
        entry.nt_flags = nt_flags;
    } else {
        fill_entry(&entry, short_name, nt_flags, attr, cluster);
    }
    uint32_t short_index = first + lfn_count;
    if (!write_slot(dir_info->first_cluster, short_index, (const uint8_t *)&entry) ||
        !slot_location(dir_info->first_cluster, short_index, &info->entry_lba, &info->entry_offset)) {
        delete_slots(dir_info->first_cluster, first, lfn_count); // No long name without its file
        return -EIO;
    }

    info->dir_index = short_index;
    info->lfn_count = (uint8_t)lfn_count;
    memcpy(info->short_name, short_name, 11);
    return 0;
}

// Has `dir` a child other than `except` whose name differs from `name`
// only in case? (To FAT those are one name.) Caller holds the tree lock.
static bool name_taken_nocase(const vnode_t *dir, const char *name, const vnode_t *except) {
    for (const vnode_t *c = dir->children; c; c = c->next) {
        if (c != except && names_equal_nocase(c->name, name)) return true;
    }
    return false;
}

// vnode_ops_t.create: make a file (no cluster until written) or a directory
// (a cluster with "." and "..") called `name` in `dir`, and attach its node.
// Returns 0, -EINVAL for a name FAT cannot hold, -EEXIST (in any case),
// -ENOSPC, -EIO or -ENOMEM.
static int fat_dir_create(vnode_t *dir, const char *name, bool directory, vnode_t **out) {
    fat_node_t *dir_info = (fat_node_t *)dir->private_data;
    if (!valid_long_name(name)) return -EINVAL;
    // "Notes.txt" and "notes.txt" are one name to FAT
    if (name_taken_nocase(dir, name, NULL)) return -EEXIST;

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
        } else if (!zero_cluster(cluster) || bcache_write(cluster_to_sector(cluster), sector, 1) != 0) {
            set_fat_entry(cluster, FAT_FREE);
            err = -EIO;
        } else {
            info->first_cluster = cluster;
        }
    }

    // Then the entry (or entries) in the parent
    if (err == 0) {
        err = store_name(dir, name, NULL, directory ? ATTR_DIRECTORY : ATTR_ARCHIVE, info->first_cluster, info, NULL);
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
    node->mode = 0755; // FAT stores no permissions: see attach_entry
    node->ops = directory ? &fat_dir_ops : &fat_file_ops;
    node->private_data = (uint64_t)info;
    node->mtime = walltime_seconds();
    *out = node;
    return 0;
}

// vnode_ops_t.remove: delete `child`'s entries (long and short) from `dir`,
// then free its clusters. Returns 0 or -EIO.
static int fat_dir_remove(vnode_t *dir, vnode_t *child) {
    const fat_node_t *dir_info = (const fat_node_t *)dir->private_data;
    fat_node_t *info = (fat_node_t *)child->private_data;
    int err = 0;

    volume_lock();
    // The short entry and the long-name entries in front of it
    if (!delete_slots(dir_info->first_cluster, info->dir_index - info->lfn_count, (uint32_t)info->lfn_count + 1)) err = -EIO;
    if (err == 0) free_chain(info->first_cluster);
    volume_unlock();

    if (err == 0) {
        kfree(info);
        child->private_data = 0;
    }
    return err;
}

// vnode_ops_t.rename: give a file or directory a new name, in the same
// directory or another one of this volume. Returns 0, -EINVAL, -EEXIST,
// -ENOSPC or -EIO. The new entries are written before the old ones are
// removed: a crash in between leaves the file under both names (which
// fsck.fat reports and repairs), never under none.
static int fat_dir_rename(vnode_t *old_dir, vnode_t *node, vnode_t *new_dir, const char *new_name) {
    const fat_node_t *old_dir_info = (const fat_node_t *)old_dir->private_data;
    const fat_node_t *new_dir_info = (const fat_node_t *)new_dir->private_data;
    fat_node_t *info = (fat_node_t *)node->private_data;
    if (!info || !valid_long_name(new_name)) return -EINVAL;
    if (name_taken_nocase(new_dir, new_name, node)) return -EEXIST;

    uint8_t sector[SECTOR];
    struct dir_entry old_entry;
    fat_node_t moved = *info;
    int err = 0;

    volume_lock();
    if (bcache_read(info->entry_lba, sector, 1) != 0) {
        err = -EIO;
    } else {
        memcpy(&old_entry, sector + info->entry_offset, sizeof(old_entry));
        err = store_name(new_dir, new_name, &old_entry, 0, 0, &moved, node);
    }
    if (err == 0) {
        // (If this fails the file has two names; better than none.)
        delete_slots(old_dir_info->first_cluster, info->dir_index - info->lfn_count, (uint32_t)info->lfn_count + 1);

        // A directory that has moved must say who its new parent is: its
        // second entry, "..", holds the parent's first cluster (0: the root)
        if (node->type == VFS_DIRECTORY && old_dir != new_dir && valid_cluster(info->first_cluster)) {
            uint64_t lba = cluster_to_sector(info->first_cluster);
            if (bcache_read(lba, sector, 1) == 0) {
                struct dir_entry *dotdot = (struct dir_entry *)(sector + sizeof(struct dir_entry));
                uint32_t parent = (new_dir_info->entry_lba == 0) ? 0 : new_dir_info->first_cluster;
                dotdot->cluster_high = (uint16_t)(parent >> 16);
                dotdot->cluster_low = (uint16_t)parent;
                bcache_write(lba, sector, 1);
            }
        }
        *info = moved;
    }
    volume_unlock();
    return err;
}

static int fat_dir_load(vnode_t *dir);

// vnode_ops_t.touch: write node->mtime into the directory entry (FAT keeps
// times to two seconds, from 1980). 0 or -EIO.
static int fat_touch(vnode_t *node) {
    const fat_node_t *info = (const fat_node_t *)node->private_data;
    if (!info || info->entry_lba == 0) return 0;          // The root directory has no entry
    volume_lock();
    uint8_t sector[SECTOR];
    bool ok = bcache_read(info->entry_lba, sector, 1) == 0;
    if (ok) {
        struct dir_entry *entry = (struct dir_entry *)(sector + info->entry_offset);
        uint16_t date, time;                                // (Not through pointers into a packed struct)
        fat_from_unix(node->mtime, &date, &time);
        entry->write_date = date;
        entry->write_time = time;
        ok = bcache_write(info->entry_lba, sector, 1) == 0;
    }
    volume_unlock();
    return ok ? 0 : -EIO;
}

// stat(): FAT has no inode numbers; the first cluster is the nearest thing
// (0 for an empty file). FAT has no hard links: the count stands.
static int fat_stat(vnode_t *node, struct vfs_stat *st) {
    const fat_node_t *info = (const fat_node_t *)node->private_data;
    st->dev = VFS_DEV_FAT;
    st->ino = info ? info->first_cluster : 0;
    return 0;
}

static const vnode_ops_t fat_file_ops = { .read = fat_file_read, .write = fat_file_write, .truncate = fat_file_truncate,
                                          .stat = fat_stat, .touch = fat_touch };
static const vnode_ops_t fat_dir_ops = { .create = fat_dir_create, .remove = fat_dir_remove,
                                         .rename = fat_dir_rename, .load = fat_dir_load, .stat = fat_stat };

// --- Mounting -----------------------------------------------------------------

int fat32_mount(const char *mount_point) {
    if (mounted || !mount_point) return -1;
    if (!virtio_blk_present()) return -1;

    uint8_t boot[SECTOR];
    if (bcache_read(0, boot, 1) != 0) return -1;
    if (!fat32_parse_boot_sector(boot, virtio_blk_capacity(), &volume)) {
        kprintf("fat32: the disk does not hold a FAT32 volume this driver can read\n");
        return -1;
    }

    char leaf[VFS_NAME_MAX];
    vnode_t *parent = vfs_find_parent(mount_point, leaf);
    vnode_t *root = NULL;
    if (parent) vfs_mount_dir(parent, leaf, &root);
    if (!root || root->children || root->ops) return -1; // Must be a new or empty in-memory directory

    fat_node_t *root_info = kmalloc(sizeof(fat_node_t));
    if (!root_info) return -1;
    memset(root_info, 0, sizeof(*root_info));
    root_info->first_cluster = volume.root_cluster;
    root_info->entry_lba = 0; // The root directory has no entry of its own
    root->ops = &fat_dir_ops;
    root->private_data = (uint64_t)root_info;
    root->unloaded = true;    // Nothing is read until someone looks

    mounted = true;
    mount_register(mount_point, "fat32", "/dev/vda");
    kprintf("fat32: mounted at %s: %u clusters of %u bytes; directories are read when first used\n",
            mount_point, volume.cluster_count, volume.sectors_per_cluster * SECTOR);
    return 0;
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
            if (bcache_read(volume.fat_start + cluster / (SECTOR / 4), sector, 1) != 0) return free_count;
        }
        if ((read32(sector + (cluster % (SECTOR / 4)) * 4) & FAT_ENTRY_MASK) == FAT_FREE) free_count++;
    }
    return free_count;
}
