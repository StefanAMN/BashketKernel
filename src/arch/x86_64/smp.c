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
#include "kstack.h"
#include "task.h"

static size_t cpu_total = 1;
static volatile size_t cpus_running = 1; // The boot CPU

// Where a started CPU begins executing, on a stack the bootloader gave it.
// Other CPUs run this at the same time as each other and as the boot CPU, so
// it touches shared state only with atomic operations.
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

    // Prove that this CPU can take an exception by itself: the handler runs
    // here, finds this CPU's structure through GS, and counts it
    asm volatile("int3");

    cpu->active = true;
    __atomic_add_fetch(&cpus_running, 1, __ATOMIC_SEQ_CST);

    // Parked: no task runs here until the scheduler is ready for more than
    // one CPU (see smp.h). The CPU sleeps until another one asks it to run
    // a function (smp_call) and wakes it with an interrupt. The check is
    // made with interrupts off and "sti; hlt" is atomic, so a request that
    // arrives in between is not slept through.
    for (;;) {
        asm volatile("cli");
        if (cpu->call_pending) {
            cpu->call_fn(cpu->call_arg);
            cpu->calls_done++;
            __atomic_store_n(&cpu->call_pending, 0, __ATOMIC_RELEASE);
            continue;
        }
        asm volatile("sti\n\thlt");
    }
}

bool smp_call(uint32_t cpu_id, void (*fn)(void *), void *arg, bool wait) {
    cpu_t *cpu = cpu_get(cpu_id);
    if (!cpu || !fn || cpu_id == 0 || !cpu->active || cpu == this_cpu()) return false;

    // One request at a time per CPU: claim the slot, or wait for the
    // function running there to finish
    uint32_t expected = 0;
    while (!__atomic_compare_exchange_n(&cpu->call_pending, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
        asm volatile("pause");
    }
    cpu->call_fn = fn;
    cpu->call_arg = arg;
    __atomic_store_n(&cpu->call_pending, 1, __ATOMIC_SEQ_CST); // Publishes fn and arg

    lapic_send_ipi(cpu->lapic_id, IPI_VECTOR_WAKE);

    while (wait && __atomic_load_n(&cpu->call_pending, __ATOMIC_ACQUIRE)) asm volatile("pause");
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
        cpu->extra_argument = next_id;
        next_id++;
        __atomic_store_n(&cpu->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    // Give them a moment to arrive (they are already running, in parallel)
    uint64_t deadline = pit_get_ticks() + 20;
    while (cpus_running < cpu_total && pit_get_ticks() < deadline) asm volatile("pause");

    kprintf("SMP: %zu of %zu CPUs started; the others wait for work, only CPU 0 runs tasks\n",
            (size_t)cpus_running, cpu_total);
}

size_t smp_get_cpu_count(void) {
    return cpus_running;
}

size_t smp_get_cpu_total(void) {
    return cpu_total;
}

per_cpu_t *smp_get_per_cpu(uint32_t cpu_id) {
    return cpu_get(cpu_id);
}
