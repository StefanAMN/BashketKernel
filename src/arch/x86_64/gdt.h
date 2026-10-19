/**
 * @file gdt.h
 * @brief Global Descriptor Table (GDT) and Task State Segment (TSS) Structures
 * 
 * In x86_64 Long Mode, traditional memory segmentation (base/limit checking) 
 * is largely disabled by hardware. However, the GDT remains mandatory to define:
 * 1. CPU Privilege Levels (Ring 0 Kernel vs Ring 3 User).
 * 2. Code Segment Descriptor Flags (Long mode bit 64-bit flag).
 * 3. Data Segment Selectors.
 * 4. The Task State Segment (TSS) for handling privilege level switches (Ring 3 -> 0).
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Segment selectors. The order of the user segments is dictated by SYSRET,
// which loads SS from STAR[63:48] + 8 and CS from STAR[63:48] + 16: user data
// must sit directly below user code. (Intel SDM Vol. 2B, SYSRET.)
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28 // The boot CPU's; CPU n uses GDT_TSS + 16 * n

// Selectors as loaded in ring 3 carry RPL 3 in their low two bits
#define USER_DS (GDT_USER_DATA | 3) // 0x1B
#define USER_CS (GDT_USER_CODE | 3) // 0x23

/**
 * @brief Standard 8-byte GDT Entry Descriptor structure.
 */
struct gdt_entry {
    uint16_t limit_low;    // Lower 16 bits of segment limit (ignored in 64-bit mode)
    uint16_t base_low;     // Lower 16 bits of segment base (ignored in 64-bit mode)
    uint8_t  base_middle;  // Middle 8 bits of segment base
    uint8_t  access;       // Access byte (Present, Ring level, Executable, Direction, RW)
    uint8_t  granularity;  // Flags + High 4 bits of limit
    uint8_t  base_high;    // High 8 bits of segment base
} __attribute__((packed));

/**
 * @brief System Segment Descriptor for 64-bit TSS (16 bytes spanning 2 GDT entries).
 */
struct tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_highest; // Extra 32 bits for full 64-bit base address in long mode
    uint32_t reserved;
} __attribute__((packed));

/**
 * @brief 64-bit Task State Segment (TSS) structure per Intel SDM Volume 3.
 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;        // Ring 0 Kernel Stack Pointer (used on privilege switch)
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;        // Interrupt Stack Table 1
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/**
 * @brief GDT Pointer structure loaded into CPU via GDTR (`lgdt`).
 */
struct gdt_ptr {
    uint16_t limit;       // Size of GDT table - 1
    uint64_t base;        // 64-bit linear virtual address of GDT array
} __attribute__((packed));

/**
 * @brief Fill the GDT (null, kernel code and data, user data and code), load
 *        it on the boot CPU and reload every segment register, then set up
 *        the boot CPU's per-CPU structure and TSS (cpu_init_this). Call once,
 *        first thing at boot.
 */
void gdt_init(void);

/**
 * @brief Set the stack the calling CPU switches to when an interrupt or
 *        exception arrives in ring 3 (its TSS's RSP0). The scheduler sets it
 *        to the top of the incoming task's kernel stack at every switch.
 *        Interrupts must be off (it writes this CPU's TSS).
 */
void tss_set_rsp0(uint64_t rsp);

/**
 * @brief Load the kernel's GDT on a CPU other than the boot CPU. Follow it
 *        with cpu_init_this(), which sets GS and loads that CPU's TSS.
 */
void gdt_load_on_this_cpu(void);

/**
 * @brief Describe `tss` in the GDT slot of CPU `cpu_id` and load it into the
 *        calling CPU's task register. Call on that CPU, once.
 */
void gdt_install_tss(uint32_t cpu_id, struct tss *tss);

#endif // GDT_H
