#include "test.h"
#include "virtio_blk.h"
#include "fat32.h"
#include "kstring.h"

TEST(virtio_blk_read_write_test) {
    EXPECT_EQ(virtio_blk_init(), 0);

    uint8_t write_buf[512];
    uint8_t read_buf[512];

    memset(write_buf, 0xAB, 512);
    memset(read_buf, 0x00, 512);

    EXPECT_EQ(virtio_blk_write_sector(42, write_buf), 0);
    EXPECT_EQ(virtio_blk_read_sector(42, read_buf), 0);
    EXPECT_EQ(memcmp(write_buf, read_buf, 512), 0);
}

TEST(fat32_file_operations_test) {
    EXPECT_EQ(fat32_init(), 0);

    const char *test_msg = "Hello, FAT32 Persistent Storage!";
    size_t msg_len = strlen(test_msg);

    EXPECT_EQ(fat32_write_file("data.txt", (const uint8_t *)test_msg, msg_len), 0);

    uint8_t read_back[64] = {0};
    int bytes_read = fat32_read_file("data.txt", read_back, sizeof(read_back) - 1);
    
    EXPECT_EQ(bytes_read, (int)msg_len);
    EXPECT_EQ(strcmp((char *)read_back, test_msg), 0);
}

void test_suite_storage(void) {
    kprintf("Suite: Phase 11 Block Storage & FAT32 Filesystem\n");
    RUN_TEST(virtio_blk_read_write_test);
    RUN_TEST(fat32_file_operations_test);
}
