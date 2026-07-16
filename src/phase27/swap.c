/**
 * @file swap.c
 * @brief Implementation of Virtual Memory Swap Manager
 */

#include "swap.h"
#include "kprintf.h"
#include "string_util.h"

#define MAX_SWAP_SLOTS 1024

static bool swap_bitmap[MAX_SWAP_SLOTS];
static size_t total_slots = MAX_SWAP_SLOTS;
static size_t free_slots = MAX_SWAP_SLOTS;

void swap_init(size_t max_swap_frames) {
    if (max_swap_frames > MAX_SWAP_SLOTS) {
        max_swap_frames = MAX_SWAP_SLOTS;
    }
    total_slots = max_swap_frames;
    free_slots = max_swap_frames;
    memset(swap_bitmap, 0, sizeof(swap_bitmap));
    kprintf("Swap Init: Virtual memory swap space initialized (%zu slots / %zu MB)\n", total_slots, (total_slots * 4) / 1024);
}

uint32_t swap_out_frame(uint64_t phys_addr) {
    (void)phys_addr;
    if (free_slots == 0) return 0xFFFFFFFF;

    for (size_t i = 0; i < total_slots; i++) {
        if (!swap_bitmap[i]) {
            swap_bitmap[i] = true;
            free_slots--;
            return (uint32_t)i;
        }
    }
    return 0xFFFFFFFF;
}

uint64_t swap_in_frame(uint32_t swap_slot) {
    if (swap_slot >= total_slots || !swap_bitmap[swap_slot]) return 0;

    swap_bitmap[swap_slot] = false;
    free_slots++;
    return 0x100000; // Mock restored physical frame address
}

size_t swap_get_free_slots(void) {
    return free_slots;
}
