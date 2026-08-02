/**
 * @file smp.h
 * @brief Starting the other CPUs
 *
 * What is done: every CPU the bootloader found is started, loads the
 * kernel's GDT, IDT and page tables, enables its local APIC, reports in,
 * and is then parked (halted with interrupts off).
 *
 * What is not done: the other CPUs run no tasks. The scheduler, and every
 * place that uses "interrupts off" as its lock, assumes one CPU. Putting
 * the parked CPUs to work needs per-CPU run state, a task state segment per
 * CPU, real locks in place of interrupt masking, and TLB shootdown.
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "limine.h"

#define MAX_CPUS 32

typedef struct per_cpu {
    uint32_t cpu_id;       // Our numbering: 0 is the boot CPU
    uint32_t lapic_id;     // The hardware's identifier for it
    bool active;           // It has started and reported in
} per_cpu_t;

/**
 * @brief Start every other CPU and wait for them to report in.
 * @param response The bootloader's list of CPUs, or NULL if it gave none.
 */
void smp_init(struct LIMINE_MP(response) *response);

/**
 * @brief Number of CPUs that are running (started and reported in).
 */
size_t smp_get_cpu_count(void);

/**
 * @brief Number of CPUs the bootloader reported.
 */
size_t smp_get_cpu_total(void);

per_cpu_t *smp_get_per_cpu(uint32_t cpu_id);

#endif // SMP_H
