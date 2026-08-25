/**
 * @file slab.h
 * @brief Kernel Slab Cache Allocator Interface (kmem_cache)
 */

#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct kmem_cache kmem_cache_t;

/**
 * @brief Create a new slab cache for fixed-size objects.
 * @param name Cache identifier name.
 * @param object_size Size of each object in bytes.
 * @param align Byte alignment requirement (e.g. 8 or 16).
 * @return Pointer to newly created kmem_cache_t.
 */
kmem_cache_t *kmem_cache_create(const char *name, size_t object_size, size_t align);

/**
 * @brief Allocate an object from the specified slab cache.
 * @param cache Pointer to kmem_cache_t.
 * @return Pointer to allocated object buffer.
 */
void *kmem_cache_alloc(kmem_cache_t *cache);

/**
 * @brief Free an object back to its slab cache.
 * @param cache Pointer to kmem_cache_t.
 * @param obj Pointer to object buffer to return.
 */
void kmem_cache_free(kmem_cache_t *cache, void *obj);

/**
 * @brief Destroy a slab cache and release all allocated slab pages.
 * @param cache Pointer to kmem_cache_t.
 */
void kmem_cache_destroy(kmem_cache_t *cache);

#endif // SLAB_H
