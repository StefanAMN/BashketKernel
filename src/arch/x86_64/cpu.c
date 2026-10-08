/**
 * @file cpu.c
 * @brief CPU feature detection and protection features
 *
 * Features that limit what a kernel bug or a user program can do:
 *
 *   NX    (EFER.NXE)  pages can be marked non-executable. Limine enables it;
 *                     user stacks, heaps and data are mapped NX.
 *   WP    (CR0.WP)    ring 0 honours read-only pages too. Copy-on-write
 *                     depends on it, and it keeps the kernel from writing
 *                     over its own code.
 *   SMEP  (CR4.SMEP)  ring 0 can not execute code in user pages. Stops the
 *                     classic exploit of making the kernel jump to user code.
 *   UMIP  (CR4.UMIP)  ring 3 can not run SGDT/SIDT/SLDT/SMSW/STR, which
 *                     reveal kernel addresses.
 *
 *   SMAP  (CR4.SMAP)  ring 0 can not read or write user pages either,
 *                     except between user_access_begin() and
 *                     user_access_end() (see cpu.h). Catches kernel code
 *                     that follows a pointer into user memory by mistake.
 *
 * QEMU's default CPU model has none of SMEP, SMAP, UMIP: use -cpu max.
 */

#include "cpu.h"
#include "kprintf.h"

#define CR0_WP   (1ULL << 16)
#define CR4_UMIP (1ULL << 11)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)
#define MSR_EFER 0xC0000080
#define EFER_NXE (1ULL << 11)

static char summary[40];
bool cpu_smap_enabled = false;

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) {
    asm volatile("cpuid"
                 : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                 : "a"(leaf), "c"(subleaf));
}

static uint64_t read_efer(void) {
    return rdmsr(MSR_EFER);
}

void cpu_protection_init(void) {
    uint32_t r[4];
    bool has_smep = false, has_umip = false, has_smap = false;

    // Leaf 7 exists only if leaf 0 reports a maximum of at least 7
    cpuid(0, 0, r);
    if (r[0] >= 7) {
        cpuid(7, 0, r);
        has_smep = (r[1] >> 7) & 1;  // EBX bit 7
        has_smap = (r[1] >> 20) & 1; // EBX bit 20
        has_umip = (r[2] >> 2) & 1;  // ECX bit 2
    }

    uint64_t cr0, cr4;
    asm volatile("mov %0, cr0" : "=r"(cr0));
    asm volatile("mov %0, cr4" : "=r"(cr4));

    cr0 |= CR0_WP;
    if (has_smep) cr4 |= CR4_SMEP;
    if (has_umip) cr4 |= CR4_UMIP;
    if (has_smap) cr4 |= CR4_SMAP;

    asm volatile("mov cr0, %0" : : "r"(cr0));
    asm volatile("mov cr4, %0" : : "r"(cr4));

    if (has_smap) {
        cpu_smap_enabled = true;
        user_access_end(); // Start with the window closed
    }

    ksnprintf(summary, sizeof(summary), "%swp%s%s%s",
              (read_efer() & EFER_NXE) ? "nx " : "",
              has_smep ? " smep" : "",
              has_smap ? " smap" : "",
              has_umip ? " umip" : "");
    kprintf("CPU protections: %s\n", summary);
}

const char *cpu_protection_summary(void) {
    return summary;
}
