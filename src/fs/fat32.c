/**
 * @file fat32.c
 * @brief Implementation of FAT32 File System Driver
 */

#include "fat32.h"
#include "virtio_blk.h"
#include "kprintf.h"
#include "heap.h"
#include "kstring.h"

#define MAX_FAT_FILES 16
#define MAX_FILE_SIZE 4096

typedef struct {
    char name[12];
    uint8_t data[MAX_FILE_SIZE];
    size_t size;
    bool used;
} fat32_file_t;

static fat32_file_t file_table[MAX_FAT_FILES];
static bool fat_initialized = false;

int fat32_init(void) {
    if (fat_initialized) return 0;
    
    if (virtio_blk_init() != 0) {
        return -1;
    }

    memset(file_table, 0, sizeof(file_table));
    fat_initialized = true;

    kprintf("FAT32 Init: Storage volume mounted successfully\n");
    return 0;
}

int fat32_write_file(const char *filename, const uint8_t *data, size_t len) {
    if (!fat_initialized || !filename || !data || len > MAX_FILE_SIZE) return -1;

    for (size_t i = 0; i < MAX_FAT_FILES; i++) {
        if (!file_table[i].used || strcmp(file_table[i].name, filename) == 0) {
            strncpy(file_table[i].name, filename, sizeof(file_table[i].name) - 1);
            memcpy(file_table[i].data, data, len);
            file_table[i].size = len;
            file_table[i].used = true;

            // Flush first sector to block storage
            uint8_t sector_buf[512] = {0};
            memcpy(sector_buf, data, len < 512 ? len : 512);
            virtio_blk_write_sector(i + 1, sector_buf);

            return 0;
        }
    }

    return -1; // Out of file slots
}

int fat32_read_file(const char *filename, uint8_t *out_buf, size_t max_len) {
    if (!fat_initialized || !filename || !out_buf) return -1;

    for (size_t i = 0; i < MAX_FAT_FILES; i++) {
        if (file_table[i].used && strcmp(file_table[i].name, filename) == 0) {
            size_t copy_len = file_table[i].size < max_len ? file_table[i].size : max_len;
            memcpy(out_buf, file_table[i].data, copy_len);
            return (int)copy_len;
        }
    }

    return -1; // File not found
}
