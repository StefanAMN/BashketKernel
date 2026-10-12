/**
 * @file shm.h
 * @brief Shared memory segments
 *
 * A segment is a set of page frames that several processes can map at once:
 * what one writes, the others see, with no system call in between. Processes
 * find a segment by a number they agree on (the key).
 *
 *   id = shmget(key, size)   find the segment with this key, or create it
 *   address = shmat(id)      map it into the calling process
 *   munmap(address, size)    unmap it again (exit does this too)
 *   shmrm(id)                release the key; the memory itself is freed
 *                            when the last process unmaps it
 *
 * (For memory shared only between a process and the children it forks,
 * mmap with MAP_SHARED needs no key at all.)
 *
 * Limits: at most SHM_MAX_SEGMENTS segments of at most SHM_MAX_PAGES pages,
 * and no permissions: any process can attach to or remove any segment.
 */

#ifndef SHM_H
#define SHM_H

#include <stdint.h>
#include <stddef.h>

#define SHM_MAX_SEGMENTS 16
#define SHM_MAX_PAGES 256      // 1 MiB

/**
 * @brief Find or create the segment with `key`.
 * @param size Wanted size in bytes, rounded up to whole pages. For an
 *        existing segment it must not exceed the segment's size.
 * @return The segment's id (0 or more), or -EINVAL, -ENOSPC, -ENOMEM.
 */
int shm_get(uint32_t key, size_t size);

/**
 * @brief Map a segment into an address space, at the next free place in its
 *        mmap region.
 * @return The address, or 0 on failure.
 */
uint64_t shm_attach(int id, uint64_t *pml4, uint64_t *mmap_next);

/**
 * @brief Release a segment's key and the segment table's hold on its frames.
 * @return 0, or -EINVAL if there is no such segment.
 */
int shm_remove(int id);

/**
 * @brief Whether user `uid` may attach or remove the segment: its creator
 *        and root may. (false also if there is no such segment.)
 */
bool shm_may_use(int id, uint32_t uid);

// Whether a segment with this number exists
bool shm_count_valid(int id);

/**
 * @brief Number of segments that currently exist.
 */
size_t shm_count(void);

#endif // SHM_H
