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

static per_cpu_t cpus[MAX_CPUS];
static size_t cpu_total = 1;
static volatile size_t cpus_running = 1; // The boot CPU

// Where a started CPU begins executing, on a stack the bootloader gave it.
// Other CPUs run this at the same time as each other and as the boot CPU, so
// it touches shared state only with atomic operations.
static void ap_entry(struct LIMINE_MP(info) *info) {
    // Use the kernel's own tables rather than the bootloader's
    asm volatile("mov cr3, %0" : : "r"(vmm_get_kernel_pml4_phys()) : "memory");
    gdt_load_on_this_cpu();
    idt_load_on_this_cpu();
    lapic_enable_on_this_cpu();

    uint64_t index = info->extra_argument;
    if (index < MAX_CPUS) {
        cpus[index].lapic_id = lapic_get_id();
        cpus[index].active = true;
    }
    __atomic_add_fetch(&cpus_running, 1, __ATOMIC_SEQ_CST);

    // Parked: nothing may run here until the kernel is ready for more than
    // one CPU (see smp.h). Interrupts stay off, so hlt never returns.
    for (;;) asm volatile("cli\n\thlt");
}

void smp_init(struct LIMINE_MP(response) *response) {
    cpus[0].cpu_id = 0;
    cpus[0].lapic_id = lapic_get_id();
    cpus[0].active = true;

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

        cpus[next_id].cpu_id = (uint32_t)next_id;
        cpu->extra_argument = next_id;
        next_id++;
        __atomic_store_n(&cpu->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    // Give them a moment to arrive (they are already running, in parallel)
    uint64_t deadline = pit_get_ticks() + 20;
    while (cpus_running < cpu_total && pit_get_ticks() < deadline) asm volatile("pause");

    kprintf("SMP: %zu of %zu CPUs started; the others are parked, only CPU 0 runs tasks\n",
            (size_t)cpus_running, cpu_total);
}

size_t smp_get_cpu_count(void) {
    return cpus_running;
}

size_t smp_get_cpu_total(void) {
    return cpu_total;
}

per_cpu_t *smp_get_per_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return NULL;
    return &cpus[cpu_id];
}
