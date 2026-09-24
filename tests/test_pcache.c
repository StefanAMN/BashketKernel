#include "test.h"
#include "pcache.h"
#include "vfs.h"
#include "pmm.h"
#include "cow.h"
#include "vmm.h"
#include "region.h"
#include "process.h"
#include "sched.h"
#include "kstring.h"
#include "kerrno.h"

// The contents of the frame at physical address `frame`
static const uint8_t *view(uint64_t frame) {
    return (const uint8_t *)(frame + pmm_get_hhdm_offset());
}

// One page of a file read twice is one frame, with an owner for the cache
// and one for each taker; past the end of the file the page is zeros; and
// once the file is gone, nothing is left behind
TEST(one_page_one_frame) {
    pcache_reclaim((size_t)-1);
    size_t free_before = test_free_frames();
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    EXPECT_TRUE(tmp != NULL);
    vnode_t *file = vfs_create_file(tmp, "pcache-one", "cached text", 11);
    EXPECT_TRUE(file != NULL);
    if (!file) return;
    EXPECT_TRUE(pcache_cacheable(file));

    uint64_t misses = pcache_stats()->misses, hits = pcache_stats()->hits;
    uint64_t first = pcache_get(file, 0);
    uint64_t second = pcache_get(file, 0);
    EXPECT_TRUE(first != 0);
    EXPECT_TRUE(first == second);
    EXPECT_EQ(pmm_get_frame_ref((void *)first), 3u);          // The cache, and each of us
    EXPECT_TRUE(memcmp(view(first), "cached text", 11) == 0);
    EXPECT_TRUE(view(first)[11] == 0 && view(first)[4095] == 0);
    EXPECT_EQ(pcache_stats()->misses, misses + 1);
    EXPECT_EQ(pcache_stats()->hits, hits + 1);
    EXPECT_EQ(file->cached_pages, 1u);

    uint64_t beyond = pcache_get(file, 3);                    // Wholly past the end
    EXPECT_TRUE(beyond != 0 && view(beyond)[0] == 0 && view(beyond)[4095] == 0);

    pmm_unref_frame((void *)first);
    pmm_unref_frame((void *)second);
    pmm_unref_frame((void *)beyond);
    EXPECT_EQ(pcache_unmapped_pages(), 2u);                   // Only the cache has them now
    EXPECT_EQ(vfs_unlink(tmp, "pcache-one"), 0);              // Gone with the file
    EXPECT_EQ(pcache_pages(), 0u);
    EXPECT_TRUE(test_free_frames() == free_before);
}

// A write to the file is seen by the next taker; a frame taken before the
// write keeps what it held (and stays alive while it is held); truncating
// drops the pages too
TEST(a_written_file_is_read_again) {
    pcache_reclaim((size_t)-1);
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    vnode_t *file = vfs_create_file(tmp, "pcache-write", "old words", 9);
    EXPECT_TRUE(file != NULL);
    if (!file) return;

    uint64_t before = pcache_get(file, 0);
    EXPECT_TRUE(before != 0 && memcmp(view(before), "old words", 9) == 0);
    EXPECT_EQ(vfs_write(file, 0, 3, "new"), 3u);
    EXPECT_EQ(file->cached_pages, 0u);
    EXPECT_EQ(pmm_get_frame_ref((void *)before), 1u);         // Ours alone: the cache let go
    uint64_t after = pcache_get(file, 0);
    EXPECT_TRUE(after != 0 && after != before);
    EXPECT_TRUE(memcmp(view(after), "new words", 9) == 0);
    EXPECT_TRUE(memcmp(view(before), "old words", 9) == 0);   // What was mapped stays as it was
    pmm_unref_frame((void *)before);
    pmm_unref_frame((void *)after);

    EXPECT_EQ(vfs_truncate(file, 3), 0);
    EXPECT_EQ(file->cached_pages, 0u);
    uint64_t cut = pcache_get(file, 0);
    EXPECT_TRUE(cut != 0 && memcmp(view(cut), "new", 3) == 0 && view(cut)[3] == 0);
    pmm_unref_frame((void *)cut);
    EXPECT_EQ(vfs_unlink(tmp, "pcache-write"), 0);
}

// Pages nobody maps go back when asked, oldest use first; a page someone
// holds is never taken
TEST(reclaim_takes_only_unmapped_pages) {
    pcache_reclaim((size_t)-1);
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    static char text[3 * 4096];
    memset(text, 'p', sizeof(text));
    vnode_t *file = vfs_create_file(tmp, "pcache-reclaim", text, sizeof(text));
    EXPECT_TRUE(file != NULL);
    if (!file) return;

    uint64_t held = pcache_get(file, 0);
    uint64_t a = pcache_get(file, 1), b = pcache_get(file, 2);
    pmm_unref_frame((void *)a);
    pmm_unref_frame((void *)b);
    EXPECT_EQ(pcache_pages(), 3u);
    EXPECT_EQ(pcache_unmapped_pages(), 2u);
    size_t free_before = pmm_get_free_frames();
    EXPECT_EQ(pcache_reclaim(1), 1u);
    EXPECT_EQ(pmm_get_free_frames(), free_before + 1);
    EXPECT_EQ(pcache_reclaim(100), 1u);                       // The other; not the held one
    EXPECT_EQ(pcache_pages(), 1u);
    EXPECT_EQ(pmm_get_frame_ref((void *)held), 2u);
    EXPECT_EQ(pcache_get(file, 0), held);                     // Still cached
    pmm_unref_frame((void *)held);
    pmm_unref_frame((void *)held);
    EXPECT_EQ(vfs_unlink(tmp, "pcache-reclaim"), 0);
    EXPECT_EQ(pcache_pages(), 0u);
}

// Devices and /proc files say something new at each read: never cached
TEST(devices_are_not_cached) {
    EXPECT_TRUE(!pcache_cacheable(vfs_find(NULL, "/dev/zero")));
    EXPECT_TRUE(!pcache_cacheable(vfs_find(NULL, "/proc/meminfo")));
    EXPECT_TRUE(!pcache_cacheable(vfs_find(NULL, "/tmp")));
    EXPECT_TRUE(!pcache_cacheable(NULL));
    EXPECT_TRUE(pcache_cacheable(vfs_find(NULL, "/bin/sleep")));
}

// Start /bin/sleep and wait until it is asleep having run its code
static task_t *start_sleep(pid_t *pid) {
    char *argv[] = { "/bin/sleep", "30", NULL };
    *pid = process_spawn("/bin/sleep", argv);
    task_t *task = *pid > 0 ? sched_find_task((uint32_t)*pid) : NULL;
    if (!task) return NULL;
    TEST_WAIT(task->state == TASK_BLOCKED &&
              vmm_get_phys(vmm_space_from_phys((uint64_t)task->pml4_phys), task->user_entry & ~0xFFFULL) != 0, 200);
    return task;
}

// Two processes running one program map one frame for the code both ran,
// and the second one's code came from the cache, not the file
TEST(two_processes_share_their_code) {
    pcache_reclaim((size_t)-1);
    sched_reap();
    pid_t first_pid = 0, second_pid = 0;
    task_t *first = start_sleep(&first_pid);
    EXPECT_TRUE(first != NULL);
    if (!first) return;
    uint64_t hits = pcache_stats()->hits;
    task_t *second = start_sleep(&second_pid);
    EXPECT_TRUE(second != NULL);
    if (second) {
        uint64_t entry = first->user_entry & ~0xFFFULL;      // (Each at its own address: ASLR)
        uint64_t frame_one = vmm_get_phys(vmm_space_from_phys((uint64_t)first->pml4_phys), entry);
        uint64_t frame_two = vmm_get_phys(vmm_space_from_phys((uint64_t)second->pml4_phys),
                                          second->user_entry & ~0xFFFULL);
        EXPECT_TRUE(frame_one != 0 && frame_one == frame_two);
        EXPECT_TRUE(pmm_get_frame_ref((void *)frame_one) >= 3);  // The cache and both
        EXPECT_TRUE(pcache_stats()->hits > hits);
        EXPECT_EQ(process_kill(second_pid, SIGKILL), 0);
        int status = -1;
        EXPECT_EQ(process_wait(second_pid, &status), second_pid);
    }
    EXPECT_EQ(process_kill(first_pid, SIGKILL), 0);
    int status = -1;
    EXPECT_EQ(process_wait(first_pid, &status), first_pid);
    sched_reap();
    EXPECT_TRUE(pcache_unmapped_pages() > 0);                 // Kept for the next run
    EXPECT_EQ(pcache_unmapped_pages(), pcache_pages());       // ...and mapped nowhere now
}

void test_suite_pcache(void) {
    kprintf("Suite: The page cache\n");
    RUN_TEST(one_page_one_frame);
    RUN_TEST(a_written_file_is_read_again);
    RUN_TEST(reclaim_takes_only_unmapped_pages);
    RUN_TEST(devices_are_not_cached);
    RUN_TEST(two_processes_share_their_code);
}
