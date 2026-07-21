/**
 * @file mmap.h
 * @brief Anonymous memory mappings and program break for user address spaces
 *
 * Every function takes the address space to operate on. Nothing here touches
 * the kernel's own mappings.
 */

#ifndef MMAP_H
#define MMAP_H

#include <stdint.h>
#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

// User address space layout
#define USER_MMAP_BASE 0x0000700000000000ULL // mmap regions grow up from here
#define USER_MMAP_END  0x00007F0000000000ULL
#define USER_BRK_MAX   (1ULL << 30)          // The heap may grow 1 GiB past the image

/**
 * @brief Back [addr, addr+length) with zeroed pages. Pages already mapped are
 * left alone. On failure nothing mapped by this call is left behind.
 * @return 0 on success, -1 on a bad range or out of memory.
 */
int user_map_range(uint64_t *pml4, uint64_t addr, size_t length, int prot);

/**
 * @brief Unmap [addr, addr+length) and free the frames behind it.
 * @return 0 on success, -1 on a bad range.
 */
int user_unmap_range(uint64_t *pml4, uint64_t addr, size_t length);

/**
 * @brief Map `length` bytes of zeroed memory at the next free mmap address.
 * @param next_addr The task's mmap cursor; advanced on success.
 * @return The address of the new mapping, or 0 on failure.
 */
uint64_t user_mmap_anon(uint64_t *pml4, uint64_t *next_addr, size_t length, int prot);

/**
 * @brief Move the program break, mapping or unmapping pages as needed.
 * @return The break after the call: `new_brk` on success, the old break on failure.
 */
uint64_t user_set_brk(uint64_t *pml4, uint64_t brk_start, uint64_t *brk_end, uint64_t new_brk);

#endif // MMAP_H
