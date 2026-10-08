/**
 * @file gdt.c
 * @brief Implementation of Global Descriptor Table and Task State Segment Initialization
 */

#include "gdt.h"
#include "string_util.h"

#include "percpu.h"

// External assembly routines: load GDTR and reload the segment registers;
// load the task register with a TSS selector
extern void gdt_flush(uint64_t gdt_ptr_addr);
extern void tss_flush(uint16_t selector);

// Five 8-byte segment descriptors, then one 16-byte TSS descriptor per CPU.
// The table is shared by all CPUs; each loads its own TSS from it.
static struct {
    struct gdt_entry entries[5];
    struct tss_entry tss_desc[MAX_CPUS];
} __attribute__((packed)) gdt_table;

static struct gdt_ptr gdt_pointer;

// The boot CPU's stack for the double fault handler (TSS IST1). A double
// fault is what the CPU raises when it cannot deliver an exception, most
// often because the kernel stack itself is bad. Running the handler on that
// same stack would fault again and reset the machine (triple fault). The
// other CPUs get theirs from the stack allocator when they are started.
static uint8_t double_fault_stack[16 * 1024] __attribute__((aligned(16)));

static void set_gdt_entry(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_table.entries[num].base_low    = (base & 0xFFFF);
    gdt_table.entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_table.entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_table.entries[num].limit_low   = (limit & 0xFFFF);
    gdt_table.entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_table.entries[num].granularity |= gran & 0xF0;
    gdt_table.entries[num].access      = access;
}

void gdt_install_tss(uint32_t cpu_id, struct tss *tss) {
    if (cpu_id >= MAX_CPUS) return;
    struct tss_entry *desc = &gdt_table.tss_desc[cpu_id];
    uint64_t base = (uint64_t)tss;
    uint32_t limit = sizeof(struct tss) - 1;

    desc->base_low      = (base & 0xFFFF);
    desc->base_middle   = (base >> 16) & 0xFF;
    desc->base_high     = (base >> 24) & 0xFF;
    desc->base_highest  = (base >> 32) & 0xFFFFFFFF;
    desc->reserved      = 0;
    desc->limit_low     = (limit & 0xFFFF);
    desc->granularity   = (limit >> 16) & 0x0F;
    desc->access        = 0x89; // Present (0x80) | 64-bit TSS, available (0x09)

    // Loading it marks the descriptor "busy", which is why each CPU needs
    // one of its own: a second load of a busy descriptor faults
    tss_flush((uint16_t)(GDT_TSS + cpu_id * sizeof(struct tss_entry)));
}

void gdt_init(void) {
    gdt_pointer.limit = sizeof(gdt_table) - 1;
    gdt_pointer.base  = (uint64_t)&gdt_table;

    // 0x00: Null Descriptor
    set_gdt_entry(0, 0, 0, 0, 0);

    // 0x08: Kernel Code Segment (64-bit, Ring 0)
    // Access: 0x9A = Present (0x80) | Ring 0 | Executable (0x08) | Read (0x02)
    // Granularity: 0x20 = Long Mode (Bit 5 set)
    set_gdt_entry(1, 0, 0xFFFFFFFF, 0x9A, 0x20);

    // 0x10: Kernel Data Segment (Ring 0)
    // Access: 0x92 = Present (0x80) | Ring 0 | Read/Write (0x02)
    set_gdt_entry(2, 0, 0xFFFFFFFF, 0x92, 0x00);

    // 0x18: User Data Segment (Ring 3)
    // Access: 0xF2 = Present (0x80) | Ring 3 (0x60) | Read/Write (0x02)
    // Must come before user code: see the SYSRET note in gdt.h.
    set_gdt_entry(3, 0, 0xFFFFFFFF, 0xF2, 0x00);

    // 0x20: User Code Segment (64-bit, Ring 3)
    // Access: 0xFA = Present (0x80) | Ring 3 (0x60) | Executable (0x08) | Read (0x02)
    set_gdt_entry(4, 0, 0xFFFFFFFF, 0xFA, 0x20);

    gdt_flush((uint64_t)&gdt_pointer);

    // Per-CPU data and the task state segment for the boot CPU. (Must come
    // after gdt_flush: reloading the segment registers resets the GS base.)
    cpu_init_this(0, (uint64_t)double_fault_stack + sizeof(double_fault_stack));
}

void gdt_load_on_this_cpu(void) {
    gdt_flush((uint64_t)&gdt_pointer);
}

void tss_set_rsp0(uint64_t rsp) {
    this_cpu()->tss.rsp0 = rsp;
}
