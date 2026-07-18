/**
 * @file smp.h
 * @brief Symmetric Multiprocessing (SMP) Subsystem
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"

#define MAX_CPUS 32

typedef struct per_cpu {
    uint32_t cpu_id;
    uint32_t lapic_id;
    bool active;
} per_cpu_t;

/**
 * @brief Initialize SMP subsystem and per-CPU data.
 */
void smp_init(void);

/**
 * @brief Get total number of active CPU cores detected.
 * @return Active CPU count.
 */
size_t smp_get_cpu_count(void);

/**
 * @brief Retrieve per-CPU context for given CPU ID.
 * @param cpu_id CPU core index.
 * @return Pointer to per_cpu_t structure.
 */
per_cpu_t *smp_get_per_cpu(uint32_t cpu_id);

#endif // SMP_H
