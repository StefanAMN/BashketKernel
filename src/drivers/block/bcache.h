/**
 * @file bcache.h
 * @brief A cache of disk sectors
 *
 * Reading a file means reading the same few sectors of the file allocation
 * table and of its directories over and over. Each of those was a request
 * to the disk. This keeps the sectors most recently read or written in
 * memory, so that only the first look at each goes to the device.
 *
 * Writes go to the disk at once ("write-through") and the cached copy is
 * updated: nothing is ever only in the cache. That keeps the order in which
 * a filesystem's writes reach the disk exactly the order in which it made
 * them, which its safety against a crash depends on. A cache that delays
 * writes is faster and has to keep that order by other means.
 *
 * The cache is direct-mapped: a sector has exactly one place it can be in,
 * chosen by its number, and a newcomer pushes out whatever was there.
 */

#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BCACHE_SECTORS 1024   // 512 KiB of cached sectors

/**
 * @brief Read `count` sectors of disk 0 starting at `lba` into the kernel
 *        buffer `buf`, from the cache where it has them. 0 on success, -1 on
 *        a disk error. May sleep.
 */
int bcache_read(uint64_t lba, void *buf, size_t count);

/**
 * @brief Write `count` sectors to disk 0 at `lba`: to the disk, then into
 *        the cache. 0 on success, -1 on a disk error (the cached copies of
 *        those sectors are then dropped). May sleep. Two writers to the
 *        same sector at once must be kept apart by the caller (filesystems
 *        hold their volume lock): the cache could end up with the older.
 */
int bcache_write(uint64_t lba, const void *buf, size_t count);

/**
 * @brief bcache_read for disk number `dev`.
 */
int bcache_read_dev(unsigned dev, uint64_t lba, void *buf, size_t count);

/**
 * @brief bcache_write for disk number `dev`.
 */
int bcache_write_dev(unsigned dev, uint64_t lba, const void *buf, size_t count);

/**
 * @brief Forget everything (the next reads all go to the disk), for tests
 *        that must see what is really there.
 */
void bcache_invalidate(void);

typedef struct {
    uint64_t hits;       // Sectors served from memory
    uint64_t misses;     // Sectors that had to be read from the disk
    uint64_t writes;     // Sectors written (always to the disk as well)
} bcache_stats_t;

/**
 * @brief Copy the counts since boot into *out.
 */
void bcache_get_stats(bcache_stats_t *out);

#endif // BCACHE_H
