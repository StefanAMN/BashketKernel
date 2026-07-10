#include "gdt.h"

// 5 standard entries + 1 TSS (which takes 2 slots in 64-bit mode)
// Index:
// 0: Null
// 1: Kernel Code (0x08)
// 2: Kernel Data (0x10)
// 3: User Data   (0x18)
// 4: User Code   (0x20)
// 5 & 6: TSS     (0x28)
struct gdt_entry gdt[7];
struct gdtr gdtr_ptr;
struct tss_entry tss;

static void set_gdt_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[index].base_low    = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high   = (base >> 24) & 0xFF;
    gdt[index].limit_low   = (limit & 0xFFFF);
    gdt[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[index].access      = access;
}

static void set_tss_entry(int index, uint64_t base, uint32_t limit) {
    // A TSS descriptor in 64-bit mode is 16 bytes, which overlaps two 8-byte gdt_entry structs.
    struct tss_descriptor* tss_desc = (struct tss_descriptor*)&gdt[index];
    tss_desc->limit_low    = (limit & 0xFFFF);
    tss_desc->base_low     = (base & 0xFFFF);
    tss_desc->base_middle  = (base >> 16) & 0xFF;
    tss_desc->access       = 0x89; // Present (1), Ring 0 (00), TSS (1001)
    tss_desc->granularity  = ((limit >> 16) & 0x0F) | 0x00;
    tss_desc->base_high    = (base >> 24) & 0xFF;
    tss_desc->base_highest = (base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved0    = 0;
}

void gdt_init(void) {
    gdtr_ptr.limit = sizeof(gdt) - 1;
    gdtr_ptr.base  = (uint64_t)&gdt;

    // 0: Null descriptor
    set_gdt_entry(0, 0, 0, 0, 0);
    
    // 1: Kernel Code (Ring 0). Access 0x9A, Granularity 0x20 (64-bit flag 'L' set, 'Sz' clear)
    set_gdt_entry(1, 0, 0, 0x9A, 0x20);

    // 2: Kernel Data (Ring 0). Access 0x92, Granularity 0x00
    set_gdt_entry(2, 0, 0, 0x92, 0x00);

    // 3: User Data (Ring 3). Access 0xF2
    set_gdt_entry(3, 0, 0, 0xF2, 0x00);

    // 4: User Code (Ring 3). Access 0xFA, Granularity 0x20 (64-bit flag 'L' set)
    set_gdt_entry(4, 0, 0, 0xFA, 0x20);

    // Initialize TSS struct to 0
    uint8_t *tss_ptr = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(struct tss_entry); i++) {
        tss_ptr[i] = 0;
    }
    tss.iopb_offset = sizeof(struct tss_entry); // No IOPB

    // 5 & 6: TSS descriptor (takes 2 slots)
    set_tss_entry(5, (uint64_t)&tss, sizeof(struct tss_entry) - 1);

    // Load the GDT and reload segments
    gdt_flush((uint64_t)&gdtr_ptr);
}
