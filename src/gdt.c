#include "gdt.h"

struct gdt_entry gdt_entries[GDT_ENTRY_COUNT];

// Defined in gdt_asm.S: loads the GDT and reloads every segment register,
// including CS (which requires a far jump/return, not a plain mov).
extern void gdt_flush(uint64_t gdtr_addr);

// flags occupies the top nibble of the granularity byte (G, D/B, L, AVL);
// the bottom nibble is filled in here from bits 16-19 of limit.
static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t flags) {
    gdt_entries[index].limit_low   = limit & 0xFFFF;
    gdt_entries[index].base_low    = base & 0xFFFF;
    gdt_entries[index].base_mid    = (base >> 16) & 0xFF;
    gdt_entries[index].access      = access;
    gdt_entries[index].granularity = ((limit >> 16) & 0x0F) | (flags & 0xF0);
    gdt_entries[index].base_high   = (base >> 24) & 0xFF;
}

void gdt_init(void) {
    // Base/limit are ignored by the CPU for CS/DS/ES/SS/FS/GS in 64-bit mode
    // (Intel SDM Vol. 3 Sec. 3.4.5 -- long mode is a flat address space).
    // They're still filled in as a full 4GB flat segment to match the
    // standard OSDev Wiki "GDT Tutorial" long-mode layout, so the table
    // reads the way any reference on this topic shows it.
    //
    // access byte: P(1) DPL(2) S(1) Type(4)
    // granularity flags nibble: G(1) D/B(1) L(1) AVL(1)
    gdt_set_entry(0, 0, 0,       0x00, 0x00); // null descriptor
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0); // kernel code: P DPL0 S code RX; G=1 L=1 (64-bit)
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0); // kernel data: P DPL0 S data RW; G=1

    struct gdt_ptr gdtr = {
        .limit = sizeof(gdt_entries) - 1,
        .base  = (uint64_t)&gdt_entries,
    };
    gdt_flush((uint64_t)&gdtr);
}
