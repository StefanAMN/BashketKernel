/**
 * @file smp.h
 * @brief Starting the other CPUs
 *
 * What is done: every CPU the bootloader found is started, loads the
 * kernel's GDT, IDT and page tables, gets its own per-CPU structure (GS),
 * task state segment and double-fault stack, enables its local APIC, takes
 * a breakpoint exception to prove it can handle one, and reports in.
 *
 * The kernel's shared structures are protected by real locks (spinlock.h).
 * Once boot is over (smp_start_scheduling) every CPU runs tasks: each has an
 * idle task and a tick from its own local APIC timer, an idle CPU is woken
 * when a task becomes runnable, and removing a kernel mapping waits for
 * every CPU to drop its cached copy.
 *
 * Device interrupts all still arrive on the boot CPU.
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "limine.h"
#include "percpu.h"

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
 * @brief Number of CPUs that were started: the bootloader's count, capped
 *        at MAX_CPUS and at those memory could be found for. CPU numbers
 *        (cpu_get) run from 0 to this minus 1.
 */
size_t smp_get_cpu_total(void);

// Interrupt vectors used between and within CPUs. WAKE is sent by another
// CPU: there is a function to run, a TLB to flush or a task to pick up.
// TIMER is each CPU's own local APIC timer (see timer.h).
#define IPI_VECTOR_WAKE  0xF0
#define IPI_VECTOR_TIMER 0xF1

/**
 * @brief Let the other CPUs run tasks. Called once, at the end of boot.
 */
void smp_start_scheduling(void);

/**
 * @brief Handler for IPI_VECTOR_WAKE (interrupts off): answer a pending TLB
 *        shootdown and run a function smp_call left for this CPU.
 */
void smp_handle_ipi(void);

/**
 * @brief Make every other CPU drop its cached page-table lookups, and wait
 *        until all have. Call after removing or tightening a mapping in the
 *        kernel half of the address space, before the memory is reused.
 */
void smp_tlb_shootdown(void);

/**
 * @brief Answer a pending request from smp_tlb_shootdown. Called wherever
 *        this CPU waits with interrupts off.
 */
void smp_tlb_service(void);

/**
 * @brief Run fn(arg) on another CPU, in an interrupt handler there (so with
 *        interrupts off: it must not sleep). Do not call while holding a
 *        lock the function, or anything else on that CPU, may need.
 * @param wait Return only when the function has finished.
 * @return false if there is no such other CPU.
 */
bool smp_call(uint32_t cpu_id, void (*fn)(void *), void *arg, bool wait);

/**
 * @brief Whether a function started with smp_call on CPU `cpu_id` has not
 *        yet finished there (false for a CPU that does not exist).
 */
bool smp_call_busy(uint32_t cpu_id);

#endif // SMP_H
