/**
 * @file cow.c
 * @brief Implementation of Copy-On-Write Frame Reference Counting
 */

#include "cow.h"
#include "pmm.h"
#include "heap.h"
#include "kprintf.h"
#include "string_util.h"

#define MAX_COW_FRAMES 65536 // 256 MB physical RAM ceiling (64K frames)
static uint16_t *frame_refcounts = NULL;

void cow_init(void) {
    size_t bytes = MAX_COW_FRAMES * sizeof(uint16_t);
    frame_refcounts = (uint16_t *)kmalloc(bytes);
    if (frame_refcounts) {
        memset(frame_refcounts, 0, bytes);
        kprintf("COW Init: Refcount table initialized for %d frames (%zu KB)\n", MAX_COW_FRAMES, bytes / 1024);
    }
}

void pmm_ref_frame(void *phys_frame) {
    if (!frame_refcounts || !phys_frame) return;

    uint64_t frame_idx = (uint64_t)phys_frame / PAGE_SIZE;
    if (frame_idx < MAX_COW_FRAMES) {
        frame_refcounts[frame_idx]++;
    }
}

void pmm_unref_frame(void *phys_frame) {
    if (!phys_frame) return;

    if (!frame_refcounts) {
        pmm_free_frame(phys_frame);
        return;
    }

    uint64_t frame_idx = (uint64_t)phys_frame / PAGE_SIZE;
    if (frame_idx < MAX_COW_FRAMES) {
        if (frame_refcounts[frame_idx] > 0) {
            frame_refcounts[frame_idx]--;
        }
        if (frame_refcounts[frame_idx] == 0) {
            pmm_free_frame(phys_frame);
        }
    } else {
        pmm_free_frame(phys_frame);
    }
}

uint16_t pmm_get_frame_ref(void *phys_frame) {
    if (!frame_refcounts || !phys_frame) return 1;

    uint64_t frame_idx = (uint64_t)phys_frame / PAGE_SIZE;
    if (frame_idx < MAX_COW_FRAMES) {
        return frame_refcounts[frame_idx];
    }
    return 1;
}
