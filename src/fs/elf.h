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
#define ET_EXEC     2   // Linked for a fixed address
#define ET_DYN      3   // Position-independent: can be loaded anywhere
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

// One loaded segment as /proc/<pid>/maps shows it: page-aligned, with the
// PROT_* bits it was mapped with (PROT_READ always)
typedef struct {
    uint64_t start, end;
    int prot;
} elf_segment_t;

#define ELF_REPORTED_SEGMENTS 4   // How many segments elf_load_program reports (the first ones)

struct vnode;

// Where the loader reads a program from. Either the whole file is in
// memory (`data`: the initrd, or a buffer), or it is read a piece at a time
// from `node`, a file with its own operations (a disk file), as needed.
// `node` may also be given with `data` (a file kept in memory): segments
// can then still be left to be read from it page by page. Untrusted either
// way: every read is checked against `size`.
typedef struct {
    const uint8_t *data;
    struct vnode *node;
    size_t size;
} elf_source_t;

/**
 * @brief Read [offset, offset + len) of a program into `out`.
 * @return false if the range is not inside the file or the read fell short.
 *         Reading `node` may sleep (a disk); with `data` it never does.
 */
bool elf_source_read(const elf_source_t *src, uint64_t offset, uint64_t len, void *out);

// A segment the loader did not load: [start, end) (page-aligned) is to show
// the file from `file_offset` (page-aligned), page by page as the program
// touches it, with these PROT_* bits. Only read-only segments whose pages
// they share with no other segment, and whose memory is all file contents
// (no zero-filled tail), are left like this.
typedef struct {
    uint64_t start, end, file_offset;
    int prot;
} elf_lazy_t;

#define ELF_LAZY_SEGMENTS 4   // At most this many segments are left to be read later; others are loaded

// What elf_load_program reports about a program it loaded
typedef struct {
    uint64_t entry;
    uint64_t image_end;                             // Highest address of any segment (the heap starts after it)
    elf_segment_t segments[ELF_REPORTED_SEGMENTS];  // For /proc/<pid>/maps
    uint32_t segment_count;
    elf_lazy_t lazy[ELF_LAZY_SEGMENTS];             // Not loaded: the caller maps them from the file
    uint32_t lazy_count;
} elf_program_t;

/**
 * @brief Load a program from `src` into `pml4` at a random address (if it
 *        is position-independent).
 *
 * With `src->node` set, read-only segments are not copied but listed in
 * `out->lazy`: the caller must map them from that file (region.h) before
 * the program runs, or the program faults on its first instruction. The
 * writable segment is loaded and relocated as before (every relocation
 * must land in it: one aimed at a page not loaded is refused). Without
 * `node` everything is loaded. Reads the file through elf_source_read
 * only where it needs it (headers, the writable segment, the relocation
 * table), so it may sleep. On failure the address space may be partly
 * populated; the caller destroys it.
 */
bool elf_load_program(uint64_t *pml4, const elf_source_t *src, elf_program_t *out);

// Position-independent executables are placed at a random page-aligned
// address in [ELF_PIE_BASE_MIN, ELF_PIE_BASE_MIN + (ELF_PIE_RANDOM_MASK + 1) pages)
#define ELF_PIE_BASE_MIN    0x100000000ULL   // 4 GiB
#define ELF_PIE_RANDOM_MASK 0x3FFFFFFu       // 26 bits of pages: a 256 GiB range

/**
 * @brief As elf_load, with control over and a report of where a
 *        position-independent executable is placed.
 * @param requested_base Page-aligned load address, or 0 to choose a random
 *        one. Ignored for a fixed-address executable.
 * @param out_base If non-NULL, receives the address the image was loaded at
 *        (0 for a fixed-address executable).
 */
bool elf_load_at(uint64_t *pml4, const void *elf_data, size_t size, uint64_t requested_base,
                 uint64_t *out_entry, uint64_t *out_image_end, uint64_t *out_base);

#endif // ELF_H
