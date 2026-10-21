/**
 * @file elf_dyn.h
 * @brief Relocation of position-independent executables
 *
 * An ordinary (ET_EXEC) program is linked for one fixed address. A
 * position-independent one (ET_DYN, "PIE") is linked as if loaded at
 * address 0 and can be placed anywhere; the difference between where it was
 * linked and where it is loaded is the "load bias".
 *
 * Its code finds things relative to its own position, so the code needs no
 * changes. Its data may contain absolute addresses, though (a table of
 * pointers to strings, say), and those were computed for address 0. The
 * linker leaves a list of them: each "relative relocation" says "the 8 bytes
 * at offset X must hold the load bias plus A". Applying that list after
 * loading is all the dynamic linking a static PIE needs.
 *
 * Being able to load a program anywhere is what makes address-space layout
 * randomisation possible (see process.c).
 */

#ifndef ELF_DYN_H
#define ELF_DYN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "elf.h"

#define DT_NULL    0
#define DT_RELA    7    // Address of the relocation table
#define DT_RELASZ  8    // Its size in bytes
#define DT_RELAENT 9    // Size of one entry

#define R_X86_64_NONE     0
#define R_X86_64_RELATIVE 8

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} __attribute__((packed)) Elf64_Dyn;

typedef struct {
    uint64_t r_offset;   // Where to write, relative to the load bias
    uint64_t r_info;     // Low 32 bits: the relocation type
    int64_t r_addend;
} __attribute__((packed)) Elf64_Rela;

/**
 * @brief Apply the relocations of a position-independent executable whose
 *        segments have just been loaded into `pml4` at `bias`.
 * @param file,size The executable (untrusted).
 * @param out_count If non-NULL, receives the number of relocations applied.
 * @return false if the relocation data is malformed, points outside the
 *         loaded image, or uses a relocation type other than "relative".
 *         A file with no relocations at all is fine.
 */
bool elf_apply_relocations(uint64_t *pml4, const void *file, size_t size, uint64_t bias, size_t *out_count);

/**
 * @brief The same, reading the dynamic section and the relocation table
 *        from `src` (a piece at a time: the file need not be in memory).
 * @param ehdr,ph The executable's header and program headers, already
 *        checked by the loader (copies: they need not point into the file).
 *        May sleep when `src` is a disk file.
 */
bool elf_apply_relocations_from(uint64_t *pml4, const elf_source_t *src, const Elf64_Ehdr *ehdr,
                                const Elf64_Phdr *ph, uint64_t bias, size_t *out_count);

#endif // ELF_DYN_H
