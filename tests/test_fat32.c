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
#include "kprintf.h"

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

// Open, write, close: the usual way a program stores a file
static int64_t store(const char *path, int flags, const void *data, size_t len) {
    file_t *file = NULL;
    int err = file_open(path, flags, &file);
    if (err != 0) return err;
    int64_t n = file_write(file, data, len);
    file_close(file);
    return n;
}

// Read a whole file into buf; returns its length or a negative error
static int64_t load(const char *path, void *buf, size_t size) {
    file_t *file = NULL;
    int err = file_open(path, O_RDONLY, &file);
    if (err != 0) return err;
    size_t total = 0;
    int64_t n;
    while (total < size && (n = file_read(file, (uint8_t *)buf + total, size - total)) > 0) total += (size_t)n;
    file_close(file);
    return (int64_t)total;
}

static int remove_file(const char *dir, const char *name) {
    return vfs_unlink(vfs_find(NULL, dir), name);
}

TEST(fat32_write_and_read_back) {
    char buf[128];

    // Create, then read through a separate open
    EXPECT_EQ(store("/disk/new.txt", O_WRONLY | O_CREAT, "hello disk", 10), 10);
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(load("/disk/new.txt", buf, sizeof(buf)), 10);
    EXPECT_EQ(memcmp(buf, "hello disk", 10), 0);
    EXPECT_EQ(vfs_find(NULL, "/disk/new.txt")->size, 10u);

    // Overwriting the middle keeps both ends
    file_t *file = NULL;
    EXPECT_EQ(file_open("/disk/new.txt", O_RDWR, &file), 0);
    EXPECT_EQ(file_seek(file, 6, SEEK_SET), 6);
    EXPECT_EQ(file_write(file, "DI", 2), 2);
    file_close(file);
    EXPECT_EQ(load("/disk/new.txt", buf, sizeof(buf)), 10);
    EXPECT_EQ(memcmp(buf, "hello DIsk", 10), 0);

    // Appending
    EXPECT_EQ(store("/disk/new.txt", O_WRONLY | O_APPEND, " + more", 7), 7);
    EXPECT_EQ(load("/disk/new.txt", buf, sizeof(buf)), 17);
    EXPECT_EQ(memcmp(buf, "hello DIsk + more", 17), 0);

    // O_TRUNC empties it
    EXPECT_EQ(store("/disk/new.txt", O_WRONLY | O_TRUNC, "short", 5), 5);
    EXPECT_EQ(load("/disk/new.txt", buf, sizeof(buf)), 5);
    EXPECT_EQ(memcmp(buf, "short", 5), 0);

    // Writing past the end leaves a gap that reads as zeros
    EXPECT_EQ(file_open("/disk/new.txt", O_RDWR, &file), 0);
    EXPECT_EQ(file_seek(file, 2000, SEEK_SET), 2000);
    EXPECT_EQ(file_write(file, "END", 3), 3);
    file_close(file);
    static uint8_t big[2100];
    EXPECT_EQ(load("/disk/new.txt", big, sizeof(big)), 2003);
    int gap_is_zero = 1;
    for (int i = 5; i < 2000; i++) gap_is_zero &= (big[i] == 0);
    EXPECT_TRUE(gap_is_zero);
    EXPECT_EQ(memcmp(big, "short", 5), 0);
    EXPECT_EQ(memcmp(big + 2000, "END", 3), 0);

    EXPECT_EQ(remove_file("/disk", "new.txt"), 0);
    EXPECT_TRUE(vfs_find(NULL, "/disk/new.txt") == NULL);
    EXPECT_EQ(remove_file("/disk", "new.txt"), -ENOENT);
}

// Every cluster a file takes must come back when it is truncated or removed.
TEST(fat32_cluster_accounting) {
    uint32_t free_at_start = fat32_free_clusters();
    EXPECT_TRUE(free_at_start > 100000);

    // 50 000 bytes in 512-byte clusters: 98 clusters. Written in odd-sized
    // pieces so that writes start and end inside clusters.
    static uint8_t data[50000], back[50000];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 31 + (i >> 9));

    file_t *file = NULL;
    EXPECT_EQ(file_open("/disk/big.bin", O_WRONLY | O_CREAT, &file), 0);
    size_t written = 0;
    while (written < sizeof(data)) {
        size_t piece = 3333;
        if (piece > sizeof(data) - written) piece = sizeof(data) - written;
        int64_t n = file_write(file, data + written, piece);
        if (n <= 0) break;
        written += (size_t)n;
    }
    file_close(file);
    EXPECT_EQ(written, sizeof(data));
    EXPECT_EQ(fat32_free_clusters(), free_at_start - 98);

    EXPECT_EQ(load("/disk/big.bin", back, sizeof(back)), (int64_t)sizeof(back));
    EXPECT_EQ(memcmp(back, data, sizeof(data)), 0);

    // One write call larger than the driver takes at once is cut short, not refused
    static uint8_t huge[100000];
    EXPECT_EQ(file_open("/disk/big.bin", O_WRONLY | O_TRUNC, &file), 0);
    EXPECT_EQ(fat32_free_clusters(), free_at_start);            // Truncation freed all 98
    int64_t n = file_write(file, huge, sizeof(huge));
    EXPECT_TRUE(n > 0 && n < (int64_t)sizeof(huge));
    file_close(file);

    EXPECT_EQ(remove_file("/disk", "big.bin"), 0);
    EXPECT_EQ(fat32_free_clusters(), free_at_start);
}

TEST(fat32_directories) {
    uint32_t free_at_start = fat32_free_clusters();

    EXPECT_EQ(vfs_create(vfs_find(NULL, "/disk"), "work", VFS_DIRECTORY, NULL), 0);
    EXPECT_EQ(vfs_create(vfs_find(NULL, "/disk"), "work", VFS_DIRECTORY, NULL), -EEXIST);
    EXPECT_EQ(store("/disk/work/a.txt", O_WRONLY | O_CREAT, "in a new directory", 18), 18);
    EXPECT_EQ(vfs_create(vfs_find(NULL, "/disk/work"), "deeper", VFS_DIRECTORY, NULL), 0);
    EXPECT_EQ(store("/disk/work/deeper/b.txt", O_WRONLY | O_CREAT, "nested", 6), 6);

    char buf[32] = {0};
    EXPECT_EQ(load("/disk/work/deeper/b.txt", buf, sizeof(buf)), 6);
    EXPECT_EQ(memcmp(buf, "nested", 6), 0);

    // A directory with something in it stays
    EXPECT_EQ(remove_file("/disk", "work"), -ENOTEMPTY);
    EXPECT_EQ(remove_file("/disk/work", "deeper"), -ENOTEMPTY);

    // More files than one cluster of directory entries holds (16 per 512
    // bytes, two of them taken by "." and ".."): the directory has to grow
    char name[32];
    for (int i = 0; i < 40; i++) {
        ksnprintf(name, sizeof(name), "/disk/work/f%d.txt", i);
        char content[8];
        size_t len = ksnprintf(content, sizeof(content), "%d", i * 7);
        EXPECT_EQ(store(name, O_WRONLY | O_CREAT, content, len), (int64_t)len);
    }
    int all_correct = 1;
    for (int i = 0; i < 40; i++) {
        ksnprintf(name, sizeof(name), "/disk/work/f%d.txt", i);
        char expected[8], got[8] = {0};
        size_t len = ksnprintf(expected, sizeof(expected), "%d", i * 7);
        all_correct &= (load(name, got, sizeof(got)) == (int64_t)len) && (memcmp(got, expected, len) == 0);
    }
    EXPECT_TRUE(all_correct);

    // Remove everything again; the space of a deleted entry is reused
    for (int i = 0; i < 40; i++) {
        ksnprintf(name, sizeof(name), "f%d.txt", i);
        EXPECT_EQ(remove_file("/disk/work", name), 0);
    }
    EXPECT_EQ(store("/disk/work/again.txt", O_WRONLY | O_CREAT, "x", 1), 1);
    EXPECT_EQ(remove_file("/disk/work", "again.txt"), 0);
    EXPECT_EQ(remove_file("/disk/work/deeper", "b.txt"), 0);
    EXPECT_EQ(remove_file("/disk/work", "deeper"), 0);
    EXPECT_EQ(remove_file("/disk/work", "a.txt"), 0);
    EXPECT_EQ(remove_file("/disk", "work"), 0);

    EXPECT_TRUE(vfs_find(NULL, "/disk/work") == NULL);
    EXPECT_EQ(fat32_free_clusters(), free_at_start);
}

// Names FAT cannot store as a short name are refused, and leave nothing behind.
TEST(fat32_name_rules) {
    file_t *file = NULL;
    static const char *const bad[] = {
        "/disk/MixedCase.txt",      // Needs a long name to keep its case
        "/disk/toolongname.txt",    // More than 8 characters before the dot
        "/disk/name.text",          // More than 3 after it
        "/disk/two.dots.txt",
        "/disk/.hidden",            // Nothing before the dot
        "/disk/sp ace.txt",
        "/disk/star*.txt",
        "/disk/plus+.txt",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        EXPECT_EQ(file_open(bad[i], O_WRONLY | O_CREAT, &file), -EINVAL);
    }
    EXPECT_EQ(vfs_create(vfs_find(NULL, "/disk"), "Bad Dir", VFS_DIRECTORY, NULL), -EINVAL);

    // These are fine: all one case, digits, permitted punctuation
    static const char *const good[] = { "/disk/a", "/disk/UPPER.TXT", "/disk/lower.txt", "/disk/d1_2-3.x", "/disk/12345678.123" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        EXPECT_EQ(file_open(good[i], O_WRONLY | O_CREAT, &file), 0);
        file_close(file);
        EXPECT_TRUE(vfs_find(NULL, good[i]) != NULL);
        const char *leaf = NULL;
        vnode_t *parent = vfs_find_parent(good[i], &leaf);
        EXPECT_EQ(vfs_unlink(parent, leaf), 0);
    }

    // An open file cannot be removed, here as anywhere
    EXPECT_EQ(file_open("/disk/held.txt", O_WRONLY | O_CREAT, &file), 0);
    EXPECT_EQ(remove_file("/disk", "held.txt"), -EBUSY);
    file_close(file);
    EXPECT_EQ(remove_file("/disk", "held.txt"), 0);
}

// The VFS's own read-only flag (no filesystem sets it at the moment)
TEST(vfs_read_only_directories) {
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    vnode_t *dir = vfs_create_dir(tmp, "locked");
    EXPECT_TRUE(dir != NULL);
    EXPECT_EQ(store("/tmp/locked/kept.txt", O_WRONLY | O_CREAT, "x", 1), 1);
    dir->read_only = true;
    vfs_find(NULL, "/tmp/locked/kept.txt")->read_only = true;

    file_t *file = NULL;
    EXPECT_EQ(file_open("/tmp/locked/new.txt", O_WRONLY | O_CREAT, &file), -EROFS);
    EXPECT_EQ(file_open("/tmp/locked/kept.txt", O_WRONLY, &file), -EROFS);
    EXPECT_EQ(vfs_unlink(dir, "kept.txt"), -EROFS);
    EXPECT_EQ(vfs_create(dir, "sub", VFS_DIRECTORY, NULL), -EROFS);
    EXPECT_EQ(file_open("/tmp/locked/kept.txt", O_RDONLY, &file), 0);   // Reading is fine
    file_close(file);

    dir->read_only = false;
    EXPECT_EQ(vfs_unlink(dir, "kept.txt"), 0);
    EXPECT_EQ(vfs_unlink(tmp, "locked"), 0);
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
    kprintf("Suite: FAT32\n");
    RUN_TEST(fat32_tree_is_mounted);
    RUN_TEST(fat32_file_contents);
    RUN_TEST(fat32_large_file_every_byte);
    RUN_TEST(fat32_write_and_read_back);
    RUN_TEST(fat32_cluster_accounting);
    RUN_TEST(fat32_directories);
    RUN_TEST(fat32_name_rules);
    RUN_TEST(vfs_read_only_directories);
    RUN_TEST(fat32_program_runs_from_disk);
    RUN_TEST(fat32_rejects_damaged_boot_sectors);
}
