/**
 * @file ext2.h
 * @brief The ext2 filesystem: files with owners, on a disk
 *
 * FAT has no place to record who a file belongs to or who may read it. ext2,
 * the classic Linux filesystem, has: each file is an "inode" holding its
 * owner, group, permission bits and times, as Unix means them. The system
 * disk is in this format and is mounted as the root ("/"), so that the
 * system and users' files keep their owners and modes across a reboot. One
 * ext2 volume can be mounted at a time.
 *
 * What is implemented: reading and writing files (up to the doubly indirect
 * block: about 64 MiB with 1 KiB blocks), creating and removing files and
 * directories, rename and move, truncation to nothing, owners, groups,
 * permission bits and modification times; symbolic links (short ones kept
 * in the inode, longer ones in a block) and hard links (each name has a
 * node of its own, brought up to date from the inode at every open).
 *
 * What is not: device files, extended
 * attributes, the hash index of large directories (a directory we change is
 * marked as not indexed, as Linux's own ext2 driver does), the triply
 * indirect block, and any journal: after a crash, run e2fsck.
 */

#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// What the superblock says, checked
typedef struct {
    uint32_t block_size;
    uint32_t inodes_count, blocks_count, first_data_block;
    uint32_t blocks_per_group, inodes_per_group;
    uint32_t inode_size, first_ino;
    uint32_t groups;
    uint32_t free_blocks, free_inodes;
} ext2_layout_t;

/**
 * @brief Read and check a superblock (the 1024 bytes at byte 1024 of the
 *        volume) of a disk of `disk_sectors` sectors, and fill *out. False
 *        for anything that is not an ext2 volume this driver can use,
 *        including one with features it does not know or one larger than
 *        the disk.
 */
bool ext2_parse_superblock(const uint8_t *superblock, uint64_t disk_sectors, ext2_layout_t *out);

/**
 * @brief Mount the ext2 volume on disk number `disk` at `mount_point`:
 *        "/" makes it the root of the whole tree (the root directory the
 *        VFS starts with, still empty, takes on the volume's root), any
 *        other path a mount point (vfs_mount_dir). Unless the volume carries
 *        the clean mark of an orderly shutdown, checks it and repairs what
 *        an interrupted write left (ext2_check) first; then removes the
 *        mark for as long as it is in use. Sleeps on the disk.
 * @return 0, or -1 if there is no usable volume or one is mounted already.
 */
int ext2_mount(const char *mount_point, unsigned disk);

// What a check found and put right (see ext2_check)
typedef struct {
    uint32_t blocks_freed, inodes_freed;     // Marked in use, referred to by nothing
    uint32_t blocks_marked, inodes_marked;   // In use, and marked free
    uint32_t links_corrected;                // Inodes whose link count did not match their names
    uint32_t sizes_corrected;                // Inodes whose size or block count did not match their blocks
    uint32_t counts_corrected;               // Free counts that did not match the bitmaps
    uint32_t conflicts;                      // Damage of a kind this check does not repair
} ext2_check_t;

/**
 * @brief Walk the whole volume and make its bitmaps, link counts, sizes and
 *        free counts agree with what is reachable from the root. Run at
 *        mount when the volume was not shut down cleanly: it is what makes
 *        an interrupted write harmless. If it finds damage it does not
 *        understand, it frees nothing. *report (if not NULL) says what it
 *        did.
 * @return 0, or -1 if nothing is mounted or there was not enough memory to
 *         look (the volume is then left as it was).
 */
int ext2_check(ext2_check_t *report);

/**
 * @brief Mark the volume as unmounted cleanly, for the next mount to see
 *        that no check is needed. Only at shutdown, once every process has
 *        ended: anything written after it would make the mark a lie.
 */
void ext2_mark_clean(void);

/**
 * @brief Does the volume carry the clean mark? (While it is in use it does
 *        not: the mark is taken away at mount.)
 */
bool ext2_volume_marked_clean(void);

/**
 * @brief Whether an ext2 volume is mounted.
 */
bool ext2_mounted(void);

/**
 * @brief Free blocks and free inodes on the mounted volume (0 if none is).
 */
uint32_t ext2_free_blocks(void);
uint32_t ext2_free_inodes(void);

#ifdef KERNEL_TESTS
struct vnode;
/**
 * @brief For tests: leave the damage interrupted writes would (a block and
 *        an inode in use that nothing names; one link too many on the given
 *        file, if any), for ext2_check to find.
 */
bool ext2_test_damage(struct vnode *file_with_extra_link);
#endif

#endif // EXT2_H
