#include "test.h"
#include "fat32.h"
#include "virtio_blk.h"
#include "vfs.h"
#include "file.h"
#include "process.h"
#include "pmm.h"
#include "sched.h"
#include "kstring.h"
#include "kerrno.h"

// The disk image is built by tools/mkfat.py from the disk/ directory:
//   readme.txt, pattern.bin (100 KiB, byte i = i*7 + (i>>8)*13),
//   notes/hello.txt, notes/empty.txt, bin/hello (the user program)

static uint8_t pattern_byte(size_t i) {
    return (uint8_t)(i * 7 + (i >> 8) * 13);
}

TEST(fat32_tree_is_mounted) {
    EXPECT_TRUE(fat32_mounted());

    vnode_t *readme = vfs_find(NULL, "/disk/readme.txt");
    vnode_t *notes = vfs_find(NULL, "/disk/notes");
    vnode_t *hello = vfs_find(NULL, "/disk/notes/hello.txt");
    vnode_t *empty = vfs_find(NULL, "/disk/notes/empty.txt");
    vnode_t *pattern = vfs_find(NULL, "/disk/pattern.bin");
    EXPECT_TRUE(readme && notes && hello && empty && pattern);
    if (!readme || !notes || !hello || !empty || !pattern) return;

    EXPECT_EQ(notes->type, VFS_DIRECTORY);
    EXPECT_EQ(hello->size, 26u);
    EXPECT_EQ(empty->size, 0u);
    EXPECT_EQ(pattern->size, 100u * 1024);

    // Names are shown in the case they were given, not as README.TXT
    EXPECT_TRUE(vfs_find(NULL, "/disk/README.TXT") == NULL);
    // "." and ".." entries on the disk did not become children
    EXPECT_TRUE(vfs_find_child(notes, ".") == NULL && vfs_find_child(notes, "..") == NULL);
}

TEST(fat32_file_contents) {
    char text[128] = {0};
    file_t *file = NULL;

    EXPECT_EQ(file_open("/disk/notes/hello.txt", O_RDONLY, &file), 0);
    EXPECT_EQ(file_read(file, text, sizeof(text) - 1), 26);
    EXPECT_EQ(strcmp(text, "hello from a subdirectory\n"), 0);
    EXPECT_EQ(file_read(file, text, sizeof(text)), 0);          // End of file
    file_close(file);

    EXPECT_EQ(file_open("/disk/readme.txt", O_RDONLY, &file), 0);
    memset(text, 0, sizeof(text));
    EXPECT_TRUE(file_read(file, text, 20) == 20);
    EXPECT_EQ(memcmp(text, "This file lives on t", 20), 0);
    file_close(file);

    EXPECT_EQ(file_open("/disk/notes/empty.txt", O_RDONLY, &file), 0);
    EXPECT_EQ(file_read(file, text, sizeof(text)), 0);
    file_close(file);
}

// 100 KiB is 200 clusters: reading it follows a long chain through the FAT.
TEST(fat32_large_file_every_byte) {
    file_t *file = NULL;
    EXPECT_EQ(file_open("/disk/pattern.bin", O_RDONLY, &file), 0);
    if (!file) return;

    // The whole file in pieces of an awkward size (not a multiple of a sector)
    static uint8_t chunk[777];
    size_t position = 0;
    int correct = 1;
    int64_t n;
    while ((n = file_read(file, chunk, sizeof(chunk))) > 0) {
        for (int64_t i = 0; i < n; i++) correct &= (chunk[i] == pattern_byte(position + (size_t)i));
        position += (size_t)n;
    }
    EXPECT_EQ(n, 0);
    EXPECT_EQ(position, 100u * 1024);
    EXPECT_TRUE(correct);

    // Jumps around the file, including across cluster and sector boundaries
    static const size_t offsets[] = { 0, 1, 511, 512, 513, 4095, 4096, 51234, 100 * 1024 - 5 };
    for (size_t k = 0; k < sizeof(offsets) / sizeof(offsets[0]); k++) {
        EXPECT_EQ(file_seek(file, (int64_t)offsets[k], SEEK_SET), (int64_t)offsets[k]);
        size_t want = 5;
        EXPECT_EQ(file_read(file, chunk, want), (int64_t)want);
        int ok = 1;
        for (size_t i = 0; i < want; i++) ok &= (chunk[i] == pattern_byte(offsets[k] + i));
        EXPECT_TRUE(ok);
    }

    // A read that runs off the end is cut short
    EXPECT_EQ(file_seek(file, 100 * 1024 - 3, SEEK_SET), 100 * 1024 - 3);
    EXPECT_EQ(file_read(file, chunk, 100), 3);
    file_close(file);
}

TEST(fat32_is_read_only) {
    file_t *file = NULL;
    EXPECT_EQ(file_open("/disk/readme.txt", O_WRONLY, &file), -EROFS);
    EXPECT_EQ(file_open("/disk/readme.txt", O_RDWR | O_TRUNC, &file), -EROFS);
    EXPECT_EQ(file_open("/disk/new.txt", O_WRONLY | O_CREAT, &file), -EROFS);
    EXPECT_EQ(file_open("/disk/notes/new.txt", O_WRONLY | O_CREAT, &file), -EROFS);
    EXPECT_EQ(vfs_unlink(vfs_find(NULL, "/disk"), "readme.txt"), -EROFS);
    EXPECT_EQ(vfs_unlink(vfs_find(NULL, "/disk/notes"), "hello.txt"), -EROFS);
    EXPECT_TRUE(vfs_create_dir(vfs_find(NULL, "/disk"), "newdir") == NULL);

    // Nothing above changed anything
    EXPECT_EQ(vfs_find(NULL, "/disk/readme.txt")->size, 109u);
    EXPECT_TRUE(vfs_find(NULL, "/disk/new.txt") == NULL);
}

// A program stored on the disk runs like one from the initrd.
TEST(fat32_program_runs_from_disk) {
    sched_reap();
    size_t free_before = pmm_get_free_frames();

    char *args[] = { "/disk/bin/hello", "from", "disk", NULL };
    pid_t pid = process_spawn("/disk/bin/hello", args);
    EXPECT_TRUE(pid > 0);
    int status = -1;
    EXPECT_EQ(process_wait(pid, &status), pid);
    EXPECT_EQ(status, 0);

    // Something that is not a program is refused without leaking anything
    EXPECT_EQ(process_spawn("/disk/readme.txt", NULL), -1);
    EXPECT_EQ(process_spawn("/disk/notes/empty.txt", NULL), -1);
    EXPECT_EQ(process_spawn("/disk/notes", NULL), -1);

    sched_reap();
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

// The boot sector comes from the disk and may say anything. Take the real
// one and break it in one way at a time: each version must be rejected.
TEST(fat32_rejects_damaged_boot_sectors) {
    static uint8_t good[512], bad[512];
    fat32_layout_t layout;
    uint64_t disk = virtio_blk_capacity();

    EXPECT_EQ(virtio_blk_read(0, good, 1), 0);
    EXPECT_TRUE(fat32_parse_boot_sector(good, disk, &layout));
    EXPECT_EQ(layout.sectors_per_cluster, 1u);
    EXPECT_TRUE(layout.cluster_count >= 65525);
    EXPECT_TRUE(layout.fat_start > 0 && layout.data_start > layout.fat_start);

#define BREAK(offset, value) do { memcpy(bad, good, 512); bad[offset] = (uint8_t)(value); \
                                  EXPECT_TRUE(!fat32_parse_boot_sector(bad, disk, &layout)); } while (0)
    BREAK(510, 0x00);      // No boot signature
    BREAK(12, 0x04);       // 1024-byte sectors
    BREAK(13, 0);          // Zero sectors per cluster (a division by zero waiting to happen)
    BREAK(13, 3);          // Not a power of two
    BREAK(14, 0); bad[15] = 0; EXPECT_TRUE(!fat32_parse_boot_sector(bad, disk, &layout)); // (already checked: reserved = 0)
    BREAK(16, 0);          // No FAT at all
    BREAK(16, 9);          // Nine FATs
    BREAK(17, 1);          // Root entry count set: that is FAT16
    BREAK(22, 1);          // 16-bit FAT size set: FAT16 again
    BREAK(35, 0x7F);       // Volume far larger than the disk
    BREAK(39, 0x7F);       // FAT larger than the volume
    memcpy(bad, good, 512);                                // Root directory at cluster 0
    memset(bad + 44, 0, 4);
    EXPECT_TRUE(!fat32_parse_boot_sector(bad, disk, &layout));
    bad[44] = 1;                                           // ... and at cluster 1: both are reserved
    EXPECT_TRUE(!fat32_parse_boot_sector(bad, disk, &layout));
    BREAK(47, 0x7F);       // Root directory beyond the last cluster
#undef BREAK

    memset(bad, 0, 512);                                   // An empty disk
    EXPECT_TRUE(!fat32_parse_boot_sector(bad, disk, &layout));
    memset(bad, 0xFF, 512);                                // Garbage
    EXPECT_TRUE(!fat32_parse_boot_sector(bad, disk, &layout));
    EXPECT_TRUE(!fat32_parse_boot_sector(good, 1000, &layout));   // A disk too small for the volume
    EXPECT_TRUE(!fat32_parse_boot_sector(NULL, disk, &layout));

    // The volume cannot be mounted a second time
    EXPECT_EQ(fat32_mount("/disk2"), -1);
}

void test_suite_fat32(void) {
    kprintf("Suite: FAT32 (read-only)\n");
    RUN_TEST(fat32_tree_is_mounted);
    RUN_TEST(fat32_file_contents);
    RUN_TEST(fat32_large_file_every_byte);
    RUN_TEST(fat32_is_read_only);
    RUN_TEST(fat32_program_runs_from_disk);
    RUN_TEST(fat32_rejects_damaged_boot_sectors);
}
