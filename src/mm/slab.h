/**
 * @file slab.h
 * @brief Slab allocator: fast allocation of many objects of one size
 *
 * kmalloc searches a list for a block that fits. A cache instead cuts whole
 * pages into equal slots for one kind of object, so allocating and freeing
 * are a few pointer moves and there is no per-object header. A page that
 * becomes completely free goes back to the frame allocator, except that a
 * cache keeps one page with free slots in reserve.
 *
 * Objects must fit in one 4 KiB page together with a small header.
 */

#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct kmem_cache kmem_cache_t;

/**
 * @brief Create a cache for objects of `object_size` bytes.
 * @param align Alignment of each object: a power of two up to 1024
 *        (values below 8 are raised to 8).
 * @return NULL if the parameters are invalid, an object would not fit in a
 *         page, or memory ran out.
 */
kmem_cache_t *kmem_cache_create(const char *name, size_t object_size, size_t align);

/**
 * @brief Allocate one object. Its contents are not cleared.
 */
void *kmem_cache_alloc(kmem_cache_t *cache);

/**
 * @brief Free an object.
 * @return false (and nothing is changed) if `obj` is not a currently
 *         allocated object of this cache.
 */
bool kmem_cache_free(kmem_cache_t *cache, void *obj);

/**
 * @brief Free every page of the cache and the cache itself.
 */
void kmem_cache_destroy(kmem_cache_t *cache);

/**
 * @brief Current usage. Any out-pointer may be NULL.
 */
void kmem_cache_stats(const kmem_cache_t *cache, size_t *objects_in_use, size_t *pages, size_t *objects_per_page);

#endif // SLAB_H
