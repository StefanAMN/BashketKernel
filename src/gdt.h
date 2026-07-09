#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// 8-byte GDT entry (for Code/Data segments)
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// 16-byte TSS Descriptor (x86_64 specific)
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_highest;
    uint32_t reserved0;
} __attribute__((packed));

// The GDTR pointer struct used for `lgdt`
struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// The actual 64-bit TSS (Task State Segment)
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

void gdt_init(void);
extern void gdt_flush(uint64_t);

#endif
