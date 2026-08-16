/**
 * @file elf_dyn.c
 * @brief Implementation of ELF Dynamic Symbol Relocations
 */

#include "elf_dyn.h"

int elf_relocate_symbol(uint32_t type, uint64_t *target, uint64_t value) {
    if (!target) return -1;

    switch (type) {
        case R_X86_64_NONE:
            return 0;
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *target = value;
            return 0;
        case R_X86_64_RELATIVE:
            *target += value;
            return 0;
        default:
            return -1;
    }
}
