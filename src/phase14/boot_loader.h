/**
 * @file boot_loader.h
 * @brief Custom x86_64 Bare-Metal Bootloader Loader Subsystem Interface
 */

#ifndef BOOT_LOADER_H
#define BOOT_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint64_t pml4_base;
    uint64_t kernel_entry;
    bool long_mode_active;
} boot_info_t;

/**
 * @brief Initialize custom bootloader state and transition tables.
 */
void boot_loader_init(void);

/**
 * @brief Verify long mode CPU capability (CPUID 0x80000001).
 * @return true if 64-bit Long Mode supported by CPU, false otherwise.
 */
bool boot_loader_check_long_mode(void);

#endif // BOOT_LOADER_H
