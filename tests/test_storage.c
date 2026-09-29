#include "test.h"
#include "virtio_blk.h"
#include "file.h"
#include "sched.h"
#include "kstring.h"
#include "kerrno.h"

// The tests use sectors at the far end of the disk, away from whatever
// filesystem is at the start. The test kernel runs with a throw-away
// snapshot of the disk image, so nothing written here is kept.

TEST(disk_is_present_and_sized) {
    EXPECT_TRUE(virtio_blk_present());
    // `make test` attaches a 64 MiB image
    EXPECT_EQ(virtio_blk_capacity(), 64ULL * 1024 * 1024 / VIRTIO_BLK_SECTOR_SIZE);
}

TEST(disk_stores_what_is_written) {
    uint64_t last = virtio_blk_capacity() - 1;
    static uint8_t out[VIRTIO_BLK_SECTOR_SIZE], in[VIRTIO_BLK_SECTOR_SIZE];

    // Two different sectors keep two different contents
    for (size_t i = 0; i < sizeof(out); i++) out[i] = (uint8_t)(i * 3 + 1);
    EXPECT_EQ(virtio_blk_write(last, out, 1), 0);
    memset(out, 0xC4, sizeof(out));
    EXPECT_EQ(virtio_blk_write(last - 1, out, 1), 0);

    memset(in, 0, sizeof(in));
    EXPECT_EQ(virtio_blk_read(last, in, 1), 0);
    int ok = 1;
    for (size_t i = 0; i < sizeof(in); i++) ok &= (in[i] == (uint8_t)(i * 3 + 1));
    EXPECT_TRUE(ok);
    EXPECT_EQ(virtio_blk_read(last - 1, in, 1), 0);
    EXPECT_EQ(in[0], 0xC4);
    EXPECT_EQ(in[511], 0xC4);

    // Overwriting replaces the old contents
    memset(out, 0x11, sizeof(out));
    EXPECT_EQ(virtio_blk_write(last, out, 1), 0);
    EXPECT_EQ(virtio_blk_read(last, in, 1), 0);
    EXPECT_EQ(in[100], 0x11);
}

TEST(disk_multi_sector_transfers) {
    // 20 sectors: more than one request's worth (8), so the transfer is split
    enum { COUNT = 20 };
    static uint8_t out[COUNT * VIRTIO_BLK_SECTOR_SIZE], in[COUNT * VIRTIO_BLK_SECTOR_SIZE];
    uint64_t start = virtio_blk_capacity() - 100;

    for (size_t i = 0; i < sizeof(out); i++) out[i] = (uint8_t)((i / VIRTIO_BLK_SECTOR_SIZE) * 17 + i);
    EXPECT_EQ(virtio_blk_write(start, out, COUNT), 0);
    memset(in, 0, sizeof(in));
    EXPECT_EQ(virtio_blk_read(start, in, COUNT), 0);
    EXPECT_EQ(memcmp(in, out, sizeof(out)), 0);

    // A read from the middle lines up with the same sectors
    EXPECT_EQ(virtio_blk_read(start + 9, in, 3), 0);
    EXPECT_EQ(memcmp(in, out + 9 * VIRTIO_BLK_SECTOR_SIZE, 3 * VIRTIO_BLK_SECTOR_SIZE), 0);
}

TEST(disk_refuses_out_of_range_requests) {
    static uint8_t buf[2 * VIRTIO_BLK_SECTOR_SIZE];
    uint64_t capacity = virtio_blk_capacity();
    uint64_t before = virtio_blk_requests();

    EXPECT_EQ(virtio_blk_read(capacity, buf, 1), -1);            // First sector past the end
    EXPECT_EQ(virtio_blk_read(capacity - 1, buf, 2), -1);        // Starts inside, ends outside
    EXPECT_EQ(virtio_blk_write(capacity, buf, 1), -1);
    EXPECT_EQ(virtio_blk_read(~0ULL, buf, 1), -1);
    EXPECT_EQ(virtio_blk_read(0, buf, (size_t)-1), -1);          // Count that would wrap
    EXPECT_EQ(virtio_blk_read(0, NULL, 1), -1);
    EXPECT_EQ(virtio_blk_read(0, buf, 0), 0);                    // Nothing to do is not an error
    EXPECT_TRUE(virtio_blk_requests() == before);                // None of these reached the device

    EXPECT_EQ(virtio_blk_read(capacity - 1, buf, 1), 0);         // The last sector itself is fine
}

TEST(disk_completions_arrive_by_interrupt) {
    static uint8_t buf[VIRTIO_BLK_SECTOR_SIZE];
    uint64_t interrupts = virtio_blk_interrupts();
    uint64_t requests = virtio_blk_requests();

    for (int i = 0; i < 50; i++) EXPECT_EQ(virtio_blk_read((uint64_t)i, buf, 1), 0);

    EXPECT_TRUE(virtio_blk_requests() == requests + 50);
    // Not necessarily one interrupt per request (a fast device can finish
    // before we go to sleep), but the interrupt path must be in use
    EXPECT_TRUE(virtio_blk_interrupts() >= interrupts + 25);
}

static volatile int workers_done;
static volatile int workers_ok;

static void disk_worker(void) {
    // Each worker owns ten sectors and checks that it reads back its own data
    static volatile int next_id = 0;
    int id = next_id++;
    uint64_t base = virtio_blk_capacity() - 400 + (uint64_t)id * 10;
    uint8_t out[VIRTIO_BLK_SECTOR_SIZE], in[VIRTIO_BLK_SECTOR_SIZE];
    int ok = 1;

    for (int round = 0; round < 15; round++) {
        for (int s = 0; s < 10; s++) {
            memset(out, id * 16 + s + round, sizeof(out));
            ok &= (virtio_blk_write(base + (uint64_t)s, out, 1) == 0);
        }
        for (int s = 0; s < 10; s++) {
            ok &= (virtio_blk_read(base + (uint64_t)s, in, 1) == 0);
            ok &= (in[0] == (uint8_t)(id * 16 + s + round) && in[511] == in[0]);
        }
    }
    if (ok) workers_ok++;
    workers_done++;
}

// Several tasks using the one request slot at once: each must wait its turn
// and none may see another's data.
TEST(disk_requests_from_several_tasks) {
    workers_done = 0;
    workers_ok = 0;
    for (int i = 0; i < 4; i++) EXPECT_TRUE(task_create("disk_worker", disk_worker) != NULL);

    for (int i = 0; i < 2000 && workers_done < 4; i++) sched_sleep_ticks(1);
    EXPECT_EQ(workers_done, 4);
    EXPECT_EQ(workers_ok, 4);
}

// /dev/vda: byte offsets that do not line up with sectors
TEST(disk_device_node) {
    file_t *file = NULL;
    EXPECT_EQ(file_open("/dev/vda", O_RDWR, &file), 0);
    if (!file) return;

    int64_t disk_bytes = (int64_t)(virtio_blk_capacity() * VIRTIO_BLK_SECTOR_SIZE);
    EXPECT_EQ(file_seek(file, 0, SEEK_END), disk_bytes);

    // Write 700 bytes starting 100 bytes before a sector boundary: parts of
    // three sectors, whose other bytes must be preserved
    int64_t base = disk_bytes - 20 * VIRTIO_BLK_SECTOR_SIZE;
    static uint8_t fill[3 * VIRTIO_BLK_SECTOR_SIZE], data[700], back[3 * VIRTIO_BLK_SECTOR_SIZE];
    memset(fill, 0xEE, sizeof(fill));
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i + 1);

    EXPECT_EQ(file_seek(file, base, SEEK_SET), base);
    EXPECT_EQ(file_write(file, fill, sizeof(fill)), (int64_t)sizeof(fill));
    EXPECT_EQ(file_seek(file, base + 412, SEEK_SET), base + 412);
    EXPECT_EQ(file_write(file, data, sizeof(data)), (int64_t)sizeof(data));

    EXPECT_EQ(file_seek(file, base, SEEK_SET), base);
    EXPECT_EQ(file_read(file, back, sizeof(back)), (int64_t)sizeof(back));
    int ok = 1;
    for (size_t i = 0; i < sizeof(back); i++) {
        uint8_t expected = (i >= 412 && i < 412 + sizeof(data)) ? (uint8_t)(i - 412 + 1) : 0xEE;
        ok &= (back[i] == expected);
    }
    EXPECT_TRUE(ok);

    // At the end of the disk: a read is cut short, then end-of-file; a write
    // past the end is refused
    EXPECT_EQ(file_seek(file, disk_bytes - 10, SEEK_SET), disk_bytes - 10);
    EXPECT_EQ(file_read(file, back, 100), 10);
    EXPECT_EQ(file_read(file, back, 100), 0);
    EXPECT_EQ(file_write(file, data, 1), -ENOSPC);

    file_close(file);
}

void test_suite_storage(void) {
    kprintf("Suite: Disk (virtio-blk)\n");
    RUN_TEST(disk_is_present_and_sized);
    RUN_TEST(disk_stores_what_is_written);
    RUN_TEST(disk_multi_sector_transfers);
    RUN_TEST(disk_refuses_out_of_range_requests);
    RUN_TEST(disk_completions_arrive_by_interrupt);
    RUN_TEST(disk_requests_from_several_tasks);
    RUN_TEST(disk_device_node);
}
