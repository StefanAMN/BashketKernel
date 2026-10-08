/**
 * @file smp.h
 * @brief Starting the other CPUs
 *
 * What is done: every CPU the bootloader found is started, loads the
 * kernel's GDT, IDT and page tables, gets its own per-CPU structure (GS),
 * task state segment and double-fault stack, enables its local APIC, takes
 * a breakpoint exception to prove it can handle one, reports in, and then
 * sleeps until asked to run a function (smp_call).
 *
 * The kernel's shared structures are protected by real locks (spinlock.h),
 * so code may run on any CPU at once; tests/test_smp.c does exactly that.
 *
 * What is not done: the other CPUs run no tasks. Putting them to work needs
 * an idle task and a timer interrupt per CPU, a way to make another CPU
 * reschedule, and TLB shootdown (see PLAN.md, A3).
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "limine.h"
#include "percpu.h"

// The per-CPU structure itself is in percpu.h
typedef cpu_t per_cpu_t;

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

// Interrupt vector that wakes a sleeping CPU so that it looks for work
#define IPI_VECTOR_WAKE 0xF0

/**
 * @brief Run fn(arg) on another CPU, with interrupts off there. The function
 *        must not sleep: that CPU has no task to switch away from.
 * @param wait Return only when the function has finished.
 * @return false if there is no such other CPU.
 */
bool smp_call(uint32_t cpu_id, void (*fn)(void *), void *arg, bool wait);

/**
 * @brief Whether a function started with smp_call is still running there.
 */
bool smp_call_busy(uint32_t cpu_id);

#endif // SMP_H
