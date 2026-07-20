/**
 * @file kstack.h
 * @brief Kernel stack allocator with guard pages
 *
 * Each kernel stack lives in its own slot of kernel virtual address space,
 * with one unmapped page directly below it. A stack overflow then hits the
 * guard page and faults, instead of silently overwriting whatever memory
 * happened to sit below the stack.
 */

#ifndef KSTACK_H
#define KSTACK_H

#include <stdint.h>
#include <stddef.h>

#define KSTACK_REGION_BASE 0xFFFFA00000000000ULL
#define KSTACK_MAX_SLOTS   1024

/**
 * @brief Allocate and map a kernel stack of KERNEL_STACK_SIZE bytes.
 * @return Virtual address of the lowest usable byte, or NULL on failure.
 */
void *kstack_alloc(void);

/**
 * @brief Unmap a stack returned by kstack_alloc and free its frames.
 */
void kstack_free(void *stack_base);

/**
 * @brief Check whether an address falls in a kernel stack guard page.
 */
int kstack_is_guard_page(uint64_t addr);

#endif // KSTACK_H
