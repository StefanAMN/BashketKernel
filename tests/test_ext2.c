#include "test.h"
#include "ext2.h"
#include "bcache.h"
#include "virtio_blk.h"
#include "vfs.h"
#include "file.h"
#include "auth.h"
#include "rtc.h"
#include "kstring.h"
#include "kprintf.h"
#include "kerrno.h"

// The image mounted at /home is built by tools/mkhome.sh (mke2fs, debugfs):
//   readme.txt            root   0644
//   alice/                1000:100 0755   notes.txt 0644, diary.txt 0600
//   bob/                  1001:100 0755   hello.txt 0644
// Its writes are thrown away when the test run ends.

static void put_text(const char *path, const char *text) {
    file_t *file = NULL;
    EXPECT_EQ(file_open(path, O_WRONLY | O_CREAT | O_TRUNC, &file), 0);
    if (!file) return;
    EXPECT_EQ(file_write(file, text, strlen(text)), (int64_t)strlen(text));
    file_close(file);
}

static bool has_text(const char *path, const char *text) {
    char buf[128] = {0};
    file_t *file = NULL;
    if (file_open(path, O_RDONLY, &file) != 0) return false;
    int64_t n = file_read(file, buf, sizeof(buf) - 1);
    file_close(file);
    return n == (int64_t)strlen(text) && memcmp(buf, text, (size_t)n) == 0;
}

static int remove_entry(const char *dir, const char *name) {
    return vfs_unlink(vfs_find(NULL, dir), name);
}

TEST(ext2_reads_the_image_with_its_owners) {
    EXPECT_TRUE(ext2_mounted());
    vnode_t *home = vfs_find(NULL, "/home");
    vnode_t *alice = vfs_find(NULL, "/home/alice");
    vnode_t *notes = vfs_find(NULL, "/home/alice/notes.txt");
    vnode_t *diary = vfs_find(NULL, "/home/alice/diary.txt");
    vnode_t *bob = vfs_find(NULL, "/home/bob");
    vnode_t *readme = vfs_find(NULL, "/home/readme.txt");
    EXPECT_TRUE(home && alice && notes && diary && bob && readme);
    if (!home || !alice || !notes || !diary || !bob || !readme) return;

    // Who each belongs to and who may do what: from the disk, not invented
    EXPECT_TRUE(home->uid == 0 && home->mode == 0755);
    EXPECT_TRUE(alice->type == VFS_DIRECTORY && alice->uid == 1000 && alice->gid == 100 && alice->mode == 0755);
    EXPECT_TRUE(notes->type == VFS_FILE && notes->uid == 1000 && notes->gid == 100 && notes->mode == 0644);
    EXPECT_TRUE(diary->uid == 1000 && diary->mode == 0600);
    EXPECT_TRUE(bob->uid == 1001 && bob->gid == 100);
    EXPECT_TRUE(readme->uid == 0 && readme->mode == 0644);
    EXPECT_EQ(notes->size, 28u);
    EXPECT_TRUE(notes->mtime != 0);

    EXPECT_TRUE(has_text("/home/alice/notes.txt", "Alice keeps her notes here.\n"));
    EXPECT_TRUE(has_text("/home/bob/hello.txt", "Bob was here.\n"));
    // "." and ".." on the disk did not become children
    EXPECT_TRUE(vfs_find_child(alice, ".") == NULL && vfs_find_child(alice, "..") == NULL);

    // And the permissions are enforced: bob may read alice's notes, not her
    // diary, and may not write in her directory; alice may do all three
    file_t *file = NULL;
    EXPECT_EQ(file_open_as(1001, NULL, "/home/alice/notes.txt", O_RDONLY, &file), 0);
    file_close(file);
    EXPECT_EQ(file_open_as(1001, NULL, "/home/alice/diary.txt", O_RDONLY, &file), -EACCES);
    EXPECT_EQ(file_open_as(1001, NULL, "/home/alice/bobs.txt", O_WRONLY | O_CREAT, &file), -EACCES);
    EXPECT_EQ(file_open_as(1000, NULL, "/home/alice/diary.txt", O_RDWR, &file), 0);
    file_close(file);
}

// A new file belongs to whoever made it, and that is on the disk
TEST(ext2_records_owner_and_mode) {
    vnode_t *alice = vfs_find(NULL, "/home/alice");
    uint32_t inodes_before = ext2_free_inodes(), blocks_before = ext2_free_blocks();

    file_t *file = NULL;
    EXPECT_EQ(file_open_as(1000, NULL, "/home/alice/mine.txt", O_WRONLY | O_CREAT, &file), 0);
    EXPECT_EQ(file_write(file, "alice wrote this", 16), 16);
    file_close(file);
    vnode_t *node = vfs_find(NULL, "/home/alice/mine.txt");
    EXPECT_TRUE(node != NULL && node->uid == 1000 && node->mode == 0644);

    // Change the mode the way chmod does, then forget and read from the disk
    if (node) {
        node->mode = 0640;
        EXPECT_EQ(node->ops->setattr(node), 0);
    }
    EXPECT_TRUE(ext2_forget_directory(alice));
    bcache_invalidate();
    node = vfs_find(NULL, "/home/alice/mine.txt");
    EXPECT_TRUE(node != NULL);
    if (node) {
        EXPECT_TRUE(node->uid == 1000 && node->mode == 0640 && node->size == 16);
        struct rtc_time now;
        rtc_read_time(&now);
        EXPECT_EQ(1980 + (node->mtime >> 25), (unsigned)now.year);   // Written now, by the clock
        EXPECT_EQ((node->mtime >> 21) & 15, (unsigned)now.month);
    }
    EXPECT_TRUE(has_text("/home/alice/mine.txt", "alice wrote this"));

    EXPECT_EQ(remove_entry("/home/alice", "mine.txt"), 0);
    EXPECT_TRUE(ext2_free_inodes() == inodes_before && ext2_free_blocks() == blocks_before);
}

static uint8_t pattern(size_t i) { return (uint8_t)(i * 13 + (i >> 9) * 7); }

// A file big enough to need a block of block numbers, and a block of those
TEST(ext2_large_files_and_holes) {
    enum { SIZE = 400 * 1024 };  // With 1 KiB blocks: 12 direct, 256 through one table, the rest through two
    static uint8_t chunk[5000];
    uint32_t blocks_before = ext2_free_blocks(), inodes_before = ext2_free_inodes();
    vnode_t *home = vfs_find(NULL, "/home");

    file_t *file = NULL;
    EXPECT_EQ(file_open("/home/big.bin", O_RDWR | O_CREAT | O_TRUNC, &file), 0);
    if (!file) return;
    for (size_t at = 0; at < SIZE; ) {
        size_t n = SIZE - at < sizeof(chunk) ? SIZE - at : sizeof(chunk);
        for (size_t i = 0; i < n; i++) chunk[i] = pattern(at + i);
        EXPECT_EQ(file_write(file, chunk, n), (int64_t)n);
        at += n;
    }
    file_close(file);
    // 400 data blocks and three blocks of numbers (one table, the table of
    // tables, and the one table that one names)
    EXPECT_TRUE(blocks_before - ext2_free_blocks() == 400 + 3);

    // Every byte, read from the disk afresh
    bcache_invalidate();
    EXPECT_EQ(file_open("/home/big.bin", O_RDWR, &file), 0);
    size_t wrong = 0, total = 0;
    int64_t n;
    while ((n = file_read(file, chunk, sizeof(chunk))) > 0) {
        for (int64_t i = 0; i < n; i++) if (chunk[i] != pattern(total + (size_t)i)) wrong++;
        total += (size_t)n;
    }
    EXPECT_EQ(total, (size_t)SIZE);
    EXPECT_EQ(wrong, (size_t)0);

    // Overwrite part of it, across a block boundary and across the edge of
    // the direct blocks; the rest is untouched
    EXPECT_EQ(file_seek(file, 12 * 1024 - 100, SEEK_SET), 12 * 1024 - 100);
    memset(chunk, 0xEE, 300);
    EXPECT_EQ(file_write(file, chunk, 300), 300);
    EXPECT_EQ(file_seek(file, 12 * 1024 - 101, SEEK_SET), 12 * 1024 - 101);
    EXPECT_EQ(file_read(file, chunk, 302), 302);
    EXPECT_TRUE(chunk[0] == pattern(12 * 1024 - 101) && chunk[1] == 0xEE && chunk[300] == 0xEE &&
                chunk[301] == pattern(12 * 1024 + 200));
    file_close(file);

    // Cut to nothing: every block comes back, the file stays
    EXPECT_EQ(file_open("/home/big.bin", O_WRONLY | O_TRUNC, &file), 0);
    EXPECT_TRUE(ext2_free_blocks() == blocks_before);
    EXPECT_TRUE(vfs_find(NULL, "/home/big.bin")->size == 0);

    // Writing far past the end leaves a hole, which takes no blocks and
    // reads as zeros
    EXPECT_EQ(file_seek(file, 100 * 1024, SEEK_SET), 100 * 1024);
    EXPECT_EQ(file_write(file, "end", 3), 3);
    file_close(file);
    EXPECT_TRUE(blocks_before - ext2_free_blocks() == 2);            // The one data block, and the table that names it
    EXPECT_EQ(file_open("/home/big.bin", O_RDONLY, &file), 0);
    EXPECT_EQ(file_seek(file, 50 * 1024, SEEK_SET), 50 * 1024);
    EXPECT_EQ(file_read(file, chunk, 16), 16);
    size_t nonzero = 0;
    for (int i = 0; i < 16; i++) nonzero += chunk[i] != 0;
    EXPECT_EQ(nonzero, (size_t)0);
    EXPECT_EQ(file_seek(file, 100 * 1024, SEEK_SET), 100 * 1024);
    EXPECT_EQ(file_read(file, chunk, 16), 3);
    EXPECT_EQ(memcmp(chunk, "end", 3), 0);
    file_close(file);

    EXPECT_EQ(vfs_unlink(home, "big.bin"), 0);
    EXPECT_TRUE(ext2_free_blocks() == blocks_before && ext2_free_inodes() == inodes_before);
}

TEST(ext2_directories) {
    uint32_t blocks_before = ext2_free_blocks(), inodes_before = ext2_free_inodes();
    vnode_t *home = vfs_find(NULL, "/home");
    vnode_t *work = NULL;
    EXPECT_EQ(vfs_create(home, "work", VFS_DIRECTORY, &work), 0);
    EXPECT_EQ(vfs_create(home, "work", VFS_DIRECTORY, NULL), -EEXIST);
    EXPECT_TRUE(work != NULL);

    // Enough entries, with names long enough, that the directory needs
    // several blocks
    char path[96], name[64];
    for (int i = 0; i < 60; i++) {
        ksnprintf(path, sizeof(path), "/home/work/a file with a fairly long name, number %d.txt", i);
        ksnprintf(name, sizeof(name), "entry %d", i);
        put_text(path, name);
    }
    EXPECT_EQ(vfs_unlink(home, "work"), -ENOTEMPTY);

    // All there after reading the directory from the disk again
    EXPECT_TRUE(ext2_forget_directory(work));
    bcache_invalidate();
    size_t found = 0;
    for (int i = 0; i < 60; i++) {
        ksnprintf(path, sizeof(path), "/home/work/a file with a fairly long name, number %d.txt", i);
        ksnprintf(name, sizeof(name), "entry %d", i);
        if (has_text(path, name)) found++;
    }
    EXPECT_EQ(found, (size_t)60);
    size_t children = 0;
    for (vnode_t *c = work->children; c; c = c->next) children++;
    EXPECT_EQ(children, (size_t)60);

    // Remove every other one, read again: the rest are intact, and new
    // entries go into the space that was freed (the directory does not grow)
    for (int i = 0; i < 60; i += 2) {
        ksnprintf(name, sizeof(name), "a file with a fairly long name, number %d.txt", i);
        EXPECT_EQ(remove_entry("/home/work", name), 0);
    }
    uint32_t blocks_now = ext2_free_blocks();
    for (int i = 0; i < 20; i++) {
        ksnprintf(path, sizeof(path), "/home/work/short %d", i);
        put_text(path, "");
    }
    EXPECT_TRUE(ext2_free_blocks() == blocks_now);
    EXPECT_TRUE(ext2_forget_directory(work));
    children = 0;
    vfs_find(NULL, "/home/work/short 0");
    for (vnode_t *c = work->children; c; c = c->next) children++;
    EXPECT_EQ(children, (size_t)50);
    EXPECT_TRUE(has_text("/home/work/a file with a fairly long name, number 31.txt", "entry 31"));

    // Names that cannot be
    file_t *file = NULL;
    static char too_long[150];
    memset(too_long, 'n', sizeof(too_long) - 1);
    memcpy(too_long, "/home/work/", 11);
    EXPECT_EQ(file_open(too_long, O_WRONLY | O_CREAT, &file), -EINVAL);
    EXPECT_EQ(file_open("/home/work/tab\there", O_WRONLY | O_CREAT, &file), -EINVAL);

    // A directory inside, and everything removed again: nothing is left
    vnode_t *inner = NULL;
    EXPECT_EQ(vfs_create(work, "inner", VFS_DIRECTORY, &inner), 0);
    put_text("/home/work/inner/deep.txt", "deep");
    EXPECT_EQ(remove_entry("/home/work", "inner"), -ENOTEMPTY);
    EXPECT_EQ(remove_entry("/home/work/inner", "deep.txt"), 0);
    EXPECT_EQ(remove_entry("/home/work", "inner"), 0);
    while (work->children) EXPECT_EQ(vfs_unlink(work, work->children->name), 0);
    EXPECT_EQ(vfs_unlink(home, "work"), 0);
    EXPECT_TRUE(ext2_free_blocks() == blocks_before && ext2_free_inodes() == inodes_before);
}

TEST(ext2_rename_and_move) {
    uint32_t blocks_before = ext2_free_blocks(), inodes_before = ext2_free_inodes();
    vnode_t *home = vfs_find(NULL, "/home");
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    vnode_t *from = NULL, *to = NULL;
    EXPECT_EQ(vfs_create(home, "from", VFS_DIRECTORY, &from), 0);
    EXPECT_EQ(vfs_create(home, "to", VFS_DIRECTORY, &to), 0);
    put_text("/home/from/a.txt", "contents of a");
    put_text("/home/from/b.txt", "contents of b");

    EXPECT_EQ(vfs_rename(from, "a.txt", from, "renamed in place"), 0);
    EXPECT_EQ(vfs_rename(from, "renamed in place", from, "b.txt"), -EEXIST);
    EXPECT_EQ(vfs_rename(from, "renamed in place", tmp, "x"), -EXDEV);
    EXPECT_EQ(vfs_rename(home, "from", from, "inside itself"), -EINVAL);
    EXPECT_EQ(vfs_rename(from, "renamed in place", to, "moved.txt"), 0);
    EXPECT_TRUE(ext2_forget_directory(from) && ext2_forget_directory(to));
    bcache_invalidate();
    EXPECT_TRUE(vfs_find(NULL, "/home/from/a.txt") == NULL && vfs_find(NULL, "/home/from/renamed in place") == NULL);
    EXPECT_TRUE(has_text("/home/to/moved.txt", "contents of a"));
    EXPECT_TRUE(has_text("/home/from/b.txt", "contents of b"));

    // A directory, with what is in it, into another
    EXPECT_EQ(vfs_rename(home, "from", to, "from, moved"), 0);
    vnode_t *moved = vfs_find(NULL, "/home/to/from, moved");
    EXPECT_TRUE(moved != NULL && ext2_forget_directory(moved) && ext2_forget_directory(to));
    bcache_invalidate();
    EXPECT_TRUE(vfs_find(NULL, "/home/from") == NULL);
    EXPECT_TRUE(has_text("/home/to/from, moved/b.txt", "contents of b"));

    EXPECT_EQ(remove_entry("/home/to/from, moved", "b.txt"), 0);
    EXPECT_EQ(remove_entry("/home/to", "from, moved"), 0);
    EXPECT_EQ(remove_entry("/home/to", "moved.txt"), 0);
    EXPECT_EQ(vfs_unlink(home, "to"), 0);
    EXPECT_TRUE(ext2_free_blocks() == blocks_before && ext2_free_inodes() == inodes_before);
}

// A superblock that is not ours to trust
TEST(ext2_rejects_volumes_it_can_not_use) {
    static uint8_t good[1024], bad[1024];
    ext2_layout_t layout;
    uint64_t sectors = virtio_blk_capacity_of(1);
    EXPECT_EQ(bcache_read_dev(1, 2, good, 2), 0);
    EXPECT_TRUE(ext2_parse_superblock(good, sectors, &layout));
    EXPECT_TRUE(layout.block_size == 1024 && layout.blocks_count == 16384 && layout.inode_size >= 128);

    struct { int offset; uint32_t value; int width; const char *what; } changes[] = {
        { 56, 0x1234, 2, "not the ext2 magic number" },
        { 24, 7, 4, "a block size of 128 KiB" },
        { 4, 0, 4, "no blocks" },
        { 0, 0, 4, "no inodes" },
        { 4, 0x7FFFFFFF, 4, "more blocks than the disk has" },
        { 32, 0, 4, "no blocks per group" },
        { 32, 100000, 4, "more blocks per group than a bitmap block has bits" },
        { 40, 0, 4, "no inodes per group" },
        { 88, 100, 2, "an inode size that is not a power of two" },
        { 88, 8192, 2, "inodes larger than a block" },
        { 20, 5, 4, "a first data block that cannot be" },
        { 96, 0x0040, 4, "an incompatible feature we do not know (extents)" },
        { 100, 0x0008, 4, "a feature that makes writing unsafe for us" },
        { 84, 3, 4, "a first inode among the reserved ones" },
    };
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        memcpy(bad, good, sizeof(bad));
        for (int b = 0; b < changes[i].width; b++) bad[changes[i].offset + b] = (uint8_t)(changes[i].value >> (8 * b));
        if (ext2_parse_superblock(bad, sectors, &layout)) kprintf("    accepted: %s\n", changes[i].what);
        EXPECT_TRUE(!ext2_parse_superblock(bad, sectors, &layout));
    }
    EXPECT_TRUE(!ext2_parse_superblock(good, 100, &layout));     // A disk smaller than the volume claims to be
    EXPECT_TRUE(!ext2_parse_superblock(NULL, sectors, &layout));
    EXPECT_EQ(ext2_mount("/home2", 1), -1);                      // Already mounted
    EXPECT_EQ(ext2_mount("/home2", 7), -1);                      // No such disk
}

// What an interrupted write leaves behind is found and put right
TEST(ext2_check_repairs_an_interrupted_write) {
    ext2_check_t report;
    uint32_t blocks = ext2_free_blocks(), inodes = ext2_free_inodes();

    // A sound volume: nothing to do, nothing changed
    EXPECT_EQ(ext2_check(&report), 0);
    EXPECT_TRUE(report.blocks_freed == 0 && report.inodes_freed == 0 && report.links_corrected == 0);
    EXPECT_TRUE(report.blocks_marked == 0 && report.inodes_marked == 0 && report.counts_corrected == 0);
    EXPECT_TRUE(report.conflicts == 0);
    EXPECT_TRUE(ext2_free_blocks() == blocks && ext2_free_inodes() == inodes);

    // As after a crash during a create and during a rename: a block alone,
    // an inode with a block that no directory names, a link count one high
    vnode_t *notes = vfs_find(NULL, "/home/alice/notes.txt");
    EXPECT_TRUE(notes != NULL && ext2_test_damage(notes));
    EXPECT_TRUE(ext2_free_blocks() == blocks - 2 && ext2_free_inodes() == inodes - 1);

    EXPECT_EQ(ext2_check(&report), 0);
    EXPECT_EQ(report.blocks_freed, 2u);
    EXPECT_EQ(report.inodes_freed, 1u);
    EXPECT_EQ(report.links_corrected, 1u);
    EXPECT_EQ(report.conflicts, 0u);
    EXPECT_TRUE(ext2_free_blocks() == blocks && ext2_free_inodes() == inodes);

    // And then it is sound again, with every file as it was
    EXPECT_EQ(ext2_check(&report), 0);
    EXPECT_TRUE(report.blocks_freed == 0 && report.inodes_freed == 0 && report.links_corrected == 0 && report.counts_corrected == 0);
    EXPECT_TRUE(has_text("/home/alice/notes.txt", "Alice keeps her notes here.\n"));
    EXPECT_TRUE(has_text("/home/bob/hello.txt", "Bob was here.\n"));

    // Files made now land on the space that was recovered, and are found
    put_text("/home/after.txt", "after the repair");
    EXPECT_TRUE(has_text("/home/after.txt", "after the repair"));
    EXPECT_EQ(remove_entry("/home", "after.txt"), 0);
    EXPECT_TRUE(ext2_free_blocks() == blocks && ext2_free_inodes() == inodes);
}

void test_suite_ext2(void) {
    kprintf("Suite: ext2\n");
    if (!ext2_mounted()) {
        kprintf("  (no ext2 volume on a second disk: skipped)\n");
        return;
    }
    RUN_TEST(ext2_reads_the_image_with_its_owners);
    RUN_TEST(ext2_records_owner_and_mode);
    RUN_TEST(ext2_large_files_and_holes);
    RUN_TEST(ext2_directories);
    RUN_TEST(ext2_rename_and_move);
    RUN_TEST(ext2_check_repairs_an_interrupted_write);
    RUN_TEST(ext2_rejects_volumes_it_can_not_use);
}
