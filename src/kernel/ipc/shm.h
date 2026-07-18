/**
 * @file shm.h
 * @brief POSIX Shared Memory IPC Interface
 */

#ifndef SHM_H
#define SHM_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Allocate a shared memory region associated with an IPC key.
 * @param key Integer identifier key.
 * @param size Allocation size in bytes.
 * @return Pointer to shared memory region.
 */
void *shm_create(uint32_t key, size_t size);

/**
 * @brief Retrieve shared memory region pointer by IPC key.
 * @param key Target identifier key.
 * @return Pointer to shared memory region or NULL if not found.
 */
void *shm_get(uint32_t key);

/**
 * @brief Destroy shared memory region.
 * @param key Target identifier key.
 * @return 0 on success, -1 on failure.
 */
int shm_destroy(uint32_t key);

#endif // SHM_H
