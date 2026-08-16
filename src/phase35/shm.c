/**
 * @file shm.c
 * @brief Implementation of POSIX Shared Memory
 */

#include "shm.h"
#include "heap.h"
#include "string_util.h"

#define MAX_SHM_REGIONS 16

typedef struct {
    uint32_t key;
    size_t size;
    void *ptr;
} shm_region_t;

static shm_region_t shm_table[MAX_SHM_REGIONS];
static size_t shm_count = 0;

void *shm_create(uint32_t key, size_t size) {
    if (shm_count >= MAX_SHM_REGIONS || size == 0) return NULL;

    // Check if key already exists
    for (size_t i = 0; i < shm_count; i++) {
        if (shm_table[i].key == key) {
            return shm_table[i].ptr;
        }
    }

    void *ptr = kmalloc(size);
    if (!ptr) return NULL;
    memset(ptr, 0, size);

    shm_table[shm_count].key = key;
    shm_table[shm_count].size = size;
    shm_table[shm_count].ptr = ptr;

    shm_count++;
    return ptr;
}

void *shm_get(uint32_t key) {
    for (size_t i = 0; i < shm_count; i++) {
        if (shm_table[i].key == key) {
            return shm_table[i].ptr;
        }
    }
    return NULL;
}

int shm_destroy(uint32_t key) {
    for (size_t i = 0; i < shm_count; i++) {
        if (shm_table[i].key == key) {
            kfree(shm_table[i].ptr);
            for (size_t j = i; j < shm_count - 1; j++) {
                shm_table[j] = shm_table[j + 1];
            }
            shm_count--;
            return 0;
        }
    }
    return -1;
}
