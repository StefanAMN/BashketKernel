/**
 * @file mmap.h
 * @brief Virtual Memory Mapping API (mmap / munmap)
 */

#ifndef MMAP_H
#define MMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

/**
 * @brief Map virtual memory pages for current process address space.
 */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);

/**
 * @brief Unmap virtual memory pages from current process address space.
 */
int munmap(void *addr, size_t length);

#endif // MMAP_H
