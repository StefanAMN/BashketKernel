/**
 * @file gdt.c
 * @brief Implementation of Global Descriptor Table and Task State Segment Initialization
 */

#include "gdt.h"
#include "string_util.h"

// External assembly routine to load GDTR and perform far jump reload of CS/SS/DS
extern void gdt_flush(uint64_t gdt_ptr_addr);
extern void tss_flush(void);

// Table containing 7 8-byte entries (Entry 5 & 6 combine for 16-byte TSS)
static struct {
    struct gdt_entry entries[5];
    struct tss_entry tss_desc;
} __attribute__((packed)) gdt_table;

static struct gdt_ptr gdt_pointer;
static struct tss sys_tss;

static void set_gdt_entry(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_table.entries[num].base_low    = (base & 0xFFFF);
    gdt_table.entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_table.entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_table.entries[num].limit_low   = (limit & 0xFFFF);
    gdt_table.entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_table.entries[num].granularity |= gran & 0xF0;
    gdt_table.entries[num].access      = access;
}

static void set_tss_entry(uint64_t base, uint32_t limit) {
    gdt_table.tss_desc.base_low      = (base & 0xFFFF);
    gdt_table.tss_desc.base_middle   = (base >> 16) & 0xFF;
    gdt_table.tss_desc.base_high     = (base >> 24) & 0xFF;
    gdt_table.tss_desc.base_highest  = (base >> 32) & 0xFFFFFFFF;
    gdt_table.tss_desc.reserved      = 0;

    gdt_table.tss_desc.limit_low     = (limit & 0xFFFF);
    gdt_table.tss_desc.granularity   = (limit >> 16) & 0x0F;

    // Access 0x89: Present (0x80) | 64-bit TSS Available (0x09)
    gdt_table.tss_desc.access        = 0x89;
    gdt_table.tss_desc.granularity  |= 0x00;
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

    // 0x18: User Code Segment (64-bit, Ring 3)
    // Access: 0xFA = Present (0x80) | Ring 3 (0x60) | Executable (0x08) | Read (0x02)
    set_gdt_entry(3, 0, 0xFFFFFFFF, 0xFA, 0x20);

    // 0x20: User Data Segment (Ring 3)
    // Access: 0xF2 = Present (0x80) | Ring 3 (0x60) | Read/Write (0x02)
    set_gdt_entry(4, 0, 0xFFFFFFFF, 0xF2, 0x00);

    // Setup TSS
    memset(&sys_tss, 0, sizeof(struct tss));
    sys_tss.iomap_base = sizeof(struct tss);
    set_tss_entry((uint64_t)&sys_tss, sizeof(struct tss) - 1);

    // Flush GDT and TSS via assembly
    gdt_flush((uint64_t)&gdt_pointer);
    tss_flush();
}

void tss_set_rsp0(uint64_t rsp) {
    sys_tss.rsp0 = rsp;
}
