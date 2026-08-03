/**
 * @file percpu.c
 * @brief Per-CPU data (see percpu.h)
 */

#include "percpu.h"
#include "cpu.h"
#include "kstring.h"
#include "spinlock.h"

#define MSR_GS_BASE        0xC0000101 // The base GS uses now
#define MSR_KERNEL_GS_BASE 0xC0000102 // What swapgs exchanges it with

static cpu_t cpus[MAX_CPUS];

cpu_t *cpu_get(uint32_t cpu_id) {
    return cpu_id < MAX_CPUS ? &cpus[cpu_id] : NULL;
}

void cpu_init_this(uint32_t cpu_id, uint64_t fault_stack_top) {
    cpu_t *cpu = &cpus[cpu_id];
    cpu->self = cpu;
    cpu->cpu_id = cpu_id;

    // We are in ring 0: GS points at our structure, and the value to hand
    // to user code at the next swapgs is 0
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    // The task state segment tells the CPU which stacks to switch to: RSP0
    // when an interrupt arrives in ring 3 (set at every task switch), and
    // IST1 for a double fault, whatever state the current stack is in
    memset(&cpu->tss, 0, sizeof(cpu->tss));
    cpu->tss.iomap_base = sizeof(struct tss);
    cpu->tss.ist1 = fault_stack_top;
    gdt_install_tss(cpu_id, &cpu->tss);

    if (cpu_id == 0) lock_debug_enable(); // this_cpu() works from here on
}

uint64_t cpu_read_gs_base(void) {
    return rdmsr(MSR_GS_BASE);
}
