/**
 * @file smp.c
 * @brief Implementation of SMP Core Manager
 */

#include "smp.h"
#include "lapic.h"
#include "kprintf.h"

static per_cpu_t cpus[MAX_CPUS];
static size_t active_cpu_count = 0;
static spinlock_t smp_lock = SPINLOCK_INIT;

void smp_init(void) {
    spinlock_acquire(&smp_lock);

    // Primary BSP Core
    cpus[0].cpu_id = 0;
    cpus[0].lapic_id = lapic_get_id();
    cpus[0].active = true;
    active_cpu_count = 1;

    spinlock_release(&smp_lock);

    kprintf("SMP Init: Primary BSP CPU %u initialized (Total Active Cores: %zu)\n", cpus[0].cpu_id, active_cpu_count);
}

size_t smp_get_cpu_count(void) {
    return active_cpu_count;
}

per_cpu_t *smp_get_per_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return NULL;
    return &cpus[cpu_id];
}
