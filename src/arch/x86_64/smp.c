/**
 * @file smp.c
 * @brief Starting the other CPUs (see smp.h)
 */

#include "smp.h"
#include "lapic.h"
#include "gdt.h"
#include "idt.h"
#include "vmm.h"
#include "pit.h"
#include "kprintf.h"
#include "spinlock.h"
#include "sched.h"
#include "syscall.h"
#include "fpu.h"
#include "cpu.h"
#include "kstack.h"
#include "task.h"

static size_t cpu_total = 1;
static volatile size_t cpus_running = 1; // The boot CPU

// Set once the boot sequence is over: from then on the other CPUs schedule
static volatile uint32_t scheduling_started = 0;

// --- Stale translations on other CPUs ----------------------------------------
//
// Each CPU caches page-table lookups in its TLB. The kernel half of every
// address space is the same on all CPUs, so when one CPU removes a kernel
// mapping, the others may go on using their cached copy of it: the memory
// behind it is about to be reused, perhaps as another task's stack at the
// same address. They have to be told, and the change is not complete until
// each has dropped its copy ("TLB shootdown").
//
// The request is a flag per CPU plus an interrupt to get its attention. A
// CPU answers from the interrupt handler, and also from every place where
// it waits with interrupts off (taking a spinlock, waiting for another CPU),
// because the CPU that asked may be holding the very lock it is waiting for.

// Drop every cached translation on the calling CPU, global pages included
static void flush_whole_tlb(void) {
    uint64_t cr4;
    asm volatile("mov %0, cr4" : "=r"(cr4));
    if (cr4 & (1ULL << 7)) {
        // Global pages survive a CR3 reload; turning CR4.PGE off and on
        // again drops those too (Intel SDM Vol. 3A, 4.10.4.1)
        asm volatile("mov cr4, %0" : : "r"(cr4 & ~(1ULL << 7)));
        asm volatile("mov cr4, %0" : : "r"(cr4));
    } else {
        uint64_t cr3;
        asm volatile("mov %0, cr3" : "=r"(cr3));
        asm volatile("mov cr3, %0" : : "r"(cr3) : "memory");
    }
}

void smp_tlb_service(void) {
    cpu_t *cpu = this_cpu();
    // Clear, then flush: a request that arrives during the flush sets the
    // flag again and is answered by the next call
    if (cpu->tlb_flush_pending && __atomic_exchange_n(&cpu->tlb_flush_pending, 0, __ATOMIC_SEQ_CST)) {
        flush_whole_tlb();
    }
}

void smp_tlb_shootdown(void) {
    if (cpu_total <= 1) return;

    uint64_t flags = irq_save();
    cpu_t *self = this_cpu();

    for (uint32_t i = 0; i < cpu_total; i++) {
        cpu_t *cpu = cpu_get(i);
        if (cpu == self || !cpu->active) continue;
        __atomic_store_n(&cpu->tlb_flush_pending, 1, __ATOMIC_SEQ_CST);
        lapic_send_ipi(cpu->lapic_id, IPI_VECTOR_WAKE);
    }
    for (uint32_t i = 0; i < cpu_total; i++) {
        cpu_t *cpu = cpu_get(i);
        if (cpu == self || !cpu->active) continue;
        while (__atomic_load_n(&cpu->tlb_flush_pending, __ATOMIC_SEQ_CST) && !kernel_halted) {
            smp_tlb_service(); // Two CPUs may be asking each other
            asm volatile("pause");
        }
    }
    irq_restore(flags);
}

// --- Running a function on another CPU ---------------------------------------

void smp_handle_ipi(void) {
    cpu_t *cpu = this_cpu();
    smp_tlb_service();
    if (cpu->call_pending == 1) {
        cpu->call_pending = 2; // Running: a second interrupt must not start it again
        cpu->call_fn(cpu->call_arg);
        cpu->calls_done++;
        __atomic_store_n(&cpu->call_pending, 0, __ATOMIC_RELEASE);
    }
    // Anything else it was sent for (a task to run) is seen by the code the
    // interrupt returns to: the idle loop looks again after every interrupt
}

// Where a started CPU begins executing, on a stack the bootloader gave it.
// Other CPUs run this at the same time as each other and as the boot CPU.
static void ap_entry(struct LIMINE_MP(info) *info) {
    uint32_t id = (uint32_t)info->extra_argument;
    cpu_t *cpu = cpu_get(id);

    // Use the kernel's own tables rather than the bootloader's
    asm volatile("mov cr3, %0" : : "r"(vmm_get_kernel_pml4_phys()) : "memory");
    gdt_load_on_this_cpu();
    cpu_init_this(id, (uint64_t)cpu->fault_stack + KERNEL_STACK_SIZE); // GS and this CPU's TSS
    idt_load_on_this_cpu();
    lapic_enable_on_this_cpu();
    cpu->lapic_id = lapic_get_id();

    // Control registers and MSRs are per CPU: the protections, the FPU and
    // the system call entry all have to be switched on here as well
    cpu_protection_enable_on_this_cpu();
    fpu_enable_on_this_cpu();
    syscall_enable_on_this_cpu();

    // Prove that this CPU can take an exception by itself: the handler runs
    // here, finds this CPU's structure through GS, and counts it
    asm volatile("int3");

    cpu->active = true;
    __atomic_add_fetch(&cpus_running, 1, __ATOMIC_SEQ_CST);

    // Wait for the boot CPU to finish setting the kernel up. Interrupts are
    // on, so this CPU already answers smp_call and TLB requests. (The check
    // is made with interrupts off and "sti; hlt" is atomic, so the wake-up
    // is not slept through.)
    for (;;) {
        asm volatile("cli");
        if (scheduling_started) break;
        asm volatile("sti\n\thlt");
    }

    // Become this CPU's idle task and start taking work
    flush_whole_tlb();
    sched_join_this_cpu();
    timer_start_on_this_cpu();
    sched_idle_loop();
}

void smp_start_scheduling(void) {
    if (cpu_total <= 1 || scheduling_started) return;

    // The other CPUs are interrupted by their own local APIC timers, which
    // count at a rate that has to be measured first (unless the boot CPU
    // did so already, for its own one-shot timer: measuring again would
    // stop that one)
    if (lapic_timer_counts_per_tick() == 0 && !lapic_timer_calibrate(timer_ticks, 5)) {
        kprintf("SMP: local APIC timer did not count; the other CPUs stay idle\n");
        return;
    }

    __atomic_store_n(&scheduling_started, 1, __ATOMIC_SEQ_CST);
    size_t online = 1;
    for (uint32_t i = 1; i < cpu_total; i++) {
        cpu_t *cpu = cpu_get(i);
        if (!cpu->active) continue;
        lapic_send_ipi(cpu->lapic_id, IPI_VECTOR_WAKE);
        uint64_t deadline = timer_ticks() + 20;
        while (!cpu->online && timer_ticks() < deadline) asm volatile("pause");
        if (cpu->online) online++;
    }
    kprintf("SMP: %zu CPUs scheduling (%s timers)\n", online, timer_is_one_shot() ? "one-shot" : "periodic");
}

bool smp_call(uint32_t cpu_id, void (*fn)(void *), void *arg, bool wait) {
    // (Reading our own CPU number with interrupts on is safe for this
    // comparison only if we are not moved meanwhile: callers that could be
    // moved are refused below by comparing again with interrupts off.)
    cpu_t *cpu = cpu_get(cpu_id);
    if (!cpu || !fn || !cpu->active) return false;

    uint64_t flags = irq_save();
    if (cpu == this_cpu()) {
        irq_restore(flags);
        return false;
    }

    // One request at a time per CPU: claim the slot, or wait for the
    // function running there to finish
    uint32_t expected = 0;
    while (!__atomic_compare_exchange_n(&cpu->call_pending, &expected, 3, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
        smp_tlb_service();
        asm volatile("pause");
    }
    cpu->call_fn = fn;
    cpu->call_arg = arg;
    __atomic_store_n(&cpu->call_pending, 1, __ATOMIC_SEQ_CST); // Publishes fn and arg

    lapic_send_ipi(cpu->lapic_id, IPI_VECTOR_WAKE);

    while (wait && __atomic_load_n(&cpu->call_pending, __ATOMIC_ACQUIRE) && !kernel_halted) {
        smp_tlb_service();
        asm volatile("pause");
    }
    irq_restore(flags);
    return true;
}

bool smp_call_busy(uint32_t cpu_id) {
    cpu_t *cpu = cpu_get(cpu_id);
    return cpu && __atomic_load_n(&cpu->call_pending, __ATOMIC_ACQUIRE) != 0;
}

void smp_init(struct LIMINE_MP(response) *response) {
    cpu_t *boot = cpu_get(0);
    boot->lapic_id = lapic_get_id();
    boot->active = true;

    if (!response || response->cpu_count <= 1) {
        kprintf("SMP: one CPU\n");
        return;
    }

    cpu_total = response->cpu_count < MAX_CPUS ? response->cpu_count : MAX_CPUS;

    // Writing the entry address into a CPU's slot is what starts it
    size_t next_id = 1;
    for (size_t i = 0; i < response->cpu_count && next_id < MAX_CPUS; i++) {
        struct LIMINE_MP(info) *cpu = response->cpus[i];
        if (cpu->lapic_id == response->bsp_lapic_id) continue; // That is us

        // Everything the new CPU needs is prepared here: it must not call
        // the allocators itself while we may be in them
        cpu_get((uint32_t)next_id)->fault_stack = kstack_alloc();
        if (!cpu_get((uint32_t)next_id)->fault_stack) break;
        if (!sched_prepare_cpu((uint32_t)next_id)) break;
        cpu->extra_argument = next_id;
        next_id++;
        __atomic_store_n(&cpu->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }
    // Only the CPUs that were given what they need were started; the
    // others (memory ran out) are not waited for or counted
    cpu_total = next_id;

    // Give them a moment to arrive (they are already running, in parallel)
    uint64_t deadline = timer_ticks() + 20;
    while (cpus_running < cpu_total && timer_ticks() < deadline) asm volatile("pause");

    kprintf("SMP: %zu of %zu CPUs started\n",
            (size_t)cpus_running, cpu_total);
}

size_t smp_get_cpu_count(void) {
    return cpus_running;
}

size_t smp_get_cpu_total(void) {
    return cpu_total;
}
