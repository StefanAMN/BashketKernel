/**
 * @file elf_dyn.h
 * @brief Dynamic ELF Relocation Loader Interface
 */

#ifndef ELF_DYN_H
#define ELF_DYN_H

#include <stdint.h>

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

/**
 * @brief Perform dynamic relocation on a target memory address.
 * @param type Relocation type (e.g. R_X86_64_RELATIVE).
 * @param target Pointer to memory address location to patch.
 * @param value Base address / symbol value offset.
 * @return 0 on success, -1 on unsupported type.
 */
int elf_relocate_symbol(uint32_t type, uint64_t *target, uint64_t value);

#endif // ELF_DYN_H
