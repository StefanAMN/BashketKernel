/**
 * @file boot_loader.c
 * @brief Implementation of Custom Bootloader Verification & Setup
 */

#include "boot_loader.h"
#include "kprintf.h"

static boot_info_t boot_info;

void boot_loader_init(void) {
    boot_info.pml4_base = 0x1000;
    boot_info.kernel_entry = 0xFFFFFFFF80000000ULL;
    boot_info.long_mode_active = boot_loader_check_long_mode();

    kprintf("boot_loader: STUB, a CPUID long-mode check (%s); Limine booted this kernel\n",
            boot_info.long_mode_active ? "YES" : "NO");
}

bool boot_loader_check_long_mode(void) {
    uint32_t eax, edx;
    // CPUID function 0x80000001 (Extended Processor Info)
    asm volatile(
        "cpuid"
        : "=a"(eax), "=d"(edx)
        : "a"(0x80000001)
        : "ebx", "ecx"
    );
    // Bit 29 of EDX indicates 64-bit Long Mode support
    return (edx & (1 << 29)) != 0;
}
