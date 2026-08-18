#include "test.h"
#include "kprintf.h"
#include "hrtimer.h"
#include "bcache.h"
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
    uint64_t before = virtio_blk_requests(0);

    EXPECT_EQ(virtio_blk_read(capacity, buf, 1), -1);            // First sector past the end
    EXPECT_EQ(virtio_blk_read(capacity - 1, buf, 2), -1);        // Starts inside, ends outside
    EXPECT_EQ(virtio_blk_write(capacity, buf, 1), -1);
    EXPECT_EQ(virtio_blk_read(~0ULL, buf, 1), -1);
    EXPECT_EQ(virtio_blk_read(0, buf, (size_t)-1), -1);          // Count that would wrap
    EXPECT_EQ(virtio_blk_read(0, NULL, 1), -1);
    EXPECT_EQ(virtio_blk_read(0, buf, 0), 0);                    // Nothing to do is not an error
    EXPECT_TRUE(virtio_blk_requests(0) == before);                // None of these reached the device

    EXPECT_EQ(virtio_blk_read(capacity - 1, buf, 1), 0);         // The last sector itself is fine
}

TEST(disk_completions_arrive_by_interrupt) {
    static uint8_t buf[VIRTIO_BLK_SECTOR_SIZE];
    uint64_t interrupts = virtio_blk_interrupts(0);
    uint64_t requests = virtio_blk_requests(0);

    for (int i = 0; i < 50; i++) EXPECT_EQ(virtio_blk_read((uint64_t)i, buf, 1), 0);

    EXPECT_TRUE(virtio_blk_requests(0) == requests + 50);
    // Not necessarily one interrupt per request (a fast device can finish
    // before we go to sleep), but the interrupt path must be in use
    EXPECT_TRUE(virtio_blk_interrupts(0) >= interrupts + 25);
}

static volatile int workers_done;
static volatile int workers_ok;

// One of several tasks writing and reading back sectors of their own at once
static void disk_worker(void) {
    // Each worker owns ten sectors and checks that it reads back its own data
    static volatile int next_id = 0;
    int id = __atomic_fetch_add(&next_id, 1, __ATOMIC_SEQ_CST); // Workers start at once on several CPUs
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

    // The device was given more than one request at a time: a task that is
    // waiting for its answer does not keep the others from asking
    EXPECT_TRUE(virtio_blk_peak_in_flight() >= 2 && virtio_blk_peak_in_flight() <= 8);
    kprintf("    most requests with the device at once: %u\n", virtio_blk_peak_in_flight());
}

// --- The sector cache ----------------------------------------------------------

TEST(cache_serves_repeated_reads_from_memory) {
    enum { SECTORS = 64, FIRST = 3000 };
    static uint8_t first[SECTORS * VIRTIO_BLK_SECTOR_SIZE], second[SECTORS * VIRTIO_BLK_SECTOR_SIZE];
    static uint8_t raw[VIRTIO_BLK_SECTOR_SIZE], changed[VIRTIO_BLK_SECTOR_SIZE];
    bcache_stats_t before, after;

    // Cold: every sector comes from the disk
    bcache_invalidate();
    bcache_get_stats(&before);
    uint64_t requests = virtio_blk_requests(0);
    EXPECT_EQ(bcache_read(FIRST, first, SECTORS), 0);
    bcache_get_stats(&after);
    EXPECT_TRUE(after.misses == before.misses + SECTORS && after.hits == before.hits);
    EXPECT_TRUE(virtio_blk_requests(0) == requests + SECTORS);

    // Warm: none does, and the data is the same
    requests = virtio_blk_requests(0);
    EXPECT_EQ(bcache_read(FIRST, second, SECTORS), 0);
    bcache_get_stats(&before);
    EXPECT_TRUE(before.hits == after.hits + SECTORS && before.misses == after.misses);
    EXPECT_TRUE(virtio_blk_requests(0) == requests);
    EXPECT_EQ(memcmp(first, second, sizeof(first)), 0);

    // A write goes to the disk at once, and the cache has the new contents
    for (size_t i = 0; i < sizeof(changed); i++) changed[i] = (uint8_t)(i * 3 + 1);
    requests = virtio_blk_requests(0);
    EXPECT_EQ(bcache_write(FIRST + 5, changed, 1), 0);
    EXPECT_TRUE(virtio_blk_requests(0) == requests + 1);
    EXPECT_EQ(virtio_blk_read(FIRST + 5, raw, 1), 0);                 // Straight from the device
    EXPECT_EQ(memcmp(raw, changed, sizeof(raw)), 0);
    requests = virtio_blk_requests(0);
    EXPECT_EQ(bcache_read(FIRST + 5, raw, 1), 0);                     // And from the cache
    EXPECT_EQ(memcmp(raw, changed, sizeof(raw)), 0);
    EXPECT_TRUE(virtio_blk_requests(0) == requests);
    EXPECT_EQ(bcache_write(FIRST + 5, first + 5 * VIRTIO_BLK_SECTOR_SIZE, 1), 0); // Put back what was there

    // Two sectors that share a place in the cache: the newer pushes the
    // older out, and each still reads correctly
    requests = virtio_blk_requests(0);
    EXPECT_EQ(bcache_read(FIRST + BCACHE_SECTORS, raw, 1), 0);
    EXPECT_EQ(bcache_read(FIRST, raw, 1), 0);
    EXPECT_TRUE(virtio_blk_requests(0) == requests + 2);
    EXPECT_EQ(memcmp(raw, first, sizeof(raw)), 0);

    // Errors are errors with a cache too, and reach nothing
    EXPECT_EQ(bcache_read(virtio_blk_capacity(), raw, 1), -1);
    EXPECT_EQ(bcache_write(virtio_blk_capacity(), raw, 1), -1);
}

// What the cache is for: the second reading of a file needs no disk at all
TEST(cache_makes_rereading_a_file_free) {
    static uint8_t buf[4096];
    uint64_t requests[2], nanoseconds[2];

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) bcache_invalidate();
        file_t *file = NULL;
        EXPECT_EQ(file_open("/disk/pattern.bin", O_RDONLY, &file), 0);
        if (!file) return;
        uint64_t start_requests = virtio_blk_requests(0), start = clock_ns();
        int64_t n, total = 0;
        while ((n = file_read(file, buf, sizeof(buf))) > 0) total += n;
        nanoseconds[pass] = clock_ns() - start;
        requests[pass] = virtio_blk_requests(0) - start_requests;
        file_close(file);
        EXPECT_EQ(total, 100 * 1024);
    }
    kprintf("    100 KiB file: %lu disk requests in %lu us, then %lu in %lu us\n",
            requests[0], nanoseconds[0] / 1000, requests[1], nanoseconds[1] / 1000);
    EXPECT_TRUE(requests[0] >= 200);          // One per sector of data, at least
    EXPECT_TRUE(requests[1] == 0);
    // (Only where the clock can tell: without a timestamp counter it
    // counts 10 ms ticks, and both readings can take "one tick")
    if (hrtimer_tsc_hz() != 0) EXPECT_TRUE(nanoseconds[1] < nanoseconds[0]);
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
    RUN_TEST(cache_serves_repeated_reads_from_memory);
    RUN_TEST(cache_makes_rereading_a_file_free);
    RUN_TEST(disk_device_node);
}
