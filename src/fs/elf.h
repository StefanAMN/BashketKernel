/**
 * @file elf.h
 * @brief ELF64 Executable Format Structure Definitions and Parser Interface
 */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ELF_MAGIC 0x464C457F // 0x7F 'E' 'L' 'F'

// e_ident[4] (class), e_ident[5] (data encoding), e_type, e_machine
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4

#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/**
 * @brief Load a static ELF64 executable into a user address space.
 * @param pml4 Target address space (from vmm_create_address_space).
 * @param elf_data The file contents (untrusted).
 * @param size Size of the file in bytes.
 * @param out_entry Receives the entry point.
 * @param out_image_end If non-NULL, receives the highest address used by any
 *        segment (where the program break starts).
 * @return true on success. On failure the address space may be partly
 *         populated; the caller destroys it.
 */
bool elf_load(uint64_t *pml4, const void *elf_data, size_t size, uint64_t *out_entry, uint64_t *out_image_end);

#endif // ELF_H
