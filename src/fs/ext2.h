/**
 * @file ext2.h
 * @brief The ext2 filesystem: files with owners, on a disk
 *
 * FAT has no place to record who a file belongs to or who may read it. ext2,
 * the classic Linux filesystem, has: each file is an "inode" holding its
 * owner, group, permission bits and times, as Unix means them. A volume in
 * this format on the second disk is mounted at /home, so that users' files
 * keep their owners and modes across a reboot.
 *
 * What is implemented: reading and writing files (up to the doubly indirect
 * block: about 64 MiB with 1 KiB blocks), creating and removing files and
 * directories, rename and move, truncation to nothing, owners, groups,
 * permission bits and modification times.
 *
 * What is not: symbolic and hard links (existing hard links are shown as
 * separate files and survive; none can be made), device files, extended
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
 *        volume). False for anything that is not an ext2 volume this driver
 *        can use, including one with features it does not know.
 */
bool ext2_parse_superblock(const uint8_t *superblock, uint64_t disk_sectors, ext2_layout_t *out);

/**
 * @brief Mount the ext2 volume on disk number `disk` at `mount_point`.
 *        0, or -1 if there is no such volume.
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
 * @brief Walk the whole volume and make its bitmaps, link counts and free
 *        counts agree with what is reachable from the root. Run at every
 *        mount: it is what makes an interrupted write harmless. If it finds
 *        damage it does not understand, it frees nothing.
 */
int ext2_check(ext2_check_t *report);

bool ext2_mounted(void);
uint32_t ext2_free_blocks(void);
uint32_t ext2_free_inodes(void);

#ifdef KERNEL_TESTS
struct vnode;
bool ext2_forget_directory(struct vnode *dir);
bool ext2_test_damage(struct vnode *file_with_extra_link);
#endif

#endif // EXT2_H
