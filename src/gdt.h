#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define KERNEL_CODE_SEL 0x08
#define KERNEL_DATA_SEL 0x10

// null, kernel code, kernel data. No TSS/user segments yet -- TSS.RSP0 only
// matters once ring 3 exists (Phase 5).
#define GDT_ENTRY_COUNT 3

struct __attribute__((packed)) gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
};

struct __attribute__((packed)) gdt_ptr {
    uint16_t limit;
    uint64_t base;
};

void gdt_init(void);

// Exposed (not static) so tests/test_gdt.c can inspect the table contents.
extern struct gdt_entry gdt_entries[GDT_ENTRY_COUNT];

#endif // GDT_H
