/**
 * @file dev.c
 * @brief Implementation of Virtual Devices (/dev)
 */

#include "dev.h"
#include "vfs.h"
#include "kprintf.h"

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint32_t get_random_u32(void) {
    uint64_t tsc = rdtsc();
    return (uint32_t)(tsc ^ (tsc >> 16) ^ (tsc >> 32));
}

void dev_init(void) {
    vfs_init();
    vnode_t *root = vfs_get_root();
    vnode_t *dev_dir = vfs_create_dir(root, "dev");

    if (dev_dir) {
        vfs_create_file(dev_dir, "null", "", 0);
        vfs_create_file(dev_dir, "zero", "\0\0\0\0", 4);
        vfs_create_file(dev_dir, "random", "RND", 3);
        kprintf("Dev Init: Mounted virtual hardware devices at '/dev'\n");
    }
}
