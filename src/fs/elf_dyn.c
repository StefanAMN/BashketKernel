/**
 * @file elf_dyn.c
 * @brief Relocation of position-independent executables (see elf_dyn.h)
 */

#include "elf_dyn.h"
#include "vmm.h"
#include "pmm.h"

#define MAX_RELOCATIONS (1u << 20)

// True if [offset, offset + length) lies inside a file of `size` bytes
static bool in_file(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

// Where in the file is the data for link-time address `vaddr`? Returns false
// unless [vaddr, vaddr + length) lies inside the file-backed part of one
// loaded segment. (`ph`: the `count` program headers, already checked by
// the loader.)
static bool vaddr_to_file_offset(const Elf64_Phdr *ph, uint16_t count, size_t size, uint64_t vaddr,
                                 uint64_t length, uint64_t *out_offset) {
    for (uint16_t i = 0; i < count; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (vaddr < ph[i].p_vaddr) continue;
        uint64_t within = vaddr - ph[i].p_vaddr;
        if (within > ph[i].p_filesz || length > ph[i].p_filesz - within) continue;

        uint64_t offset = ph[i].p_offset + within;
        if (offset < ph[i].p_offset || !in_file(offset, length, size)) return false;
        *out_offset = offset;
        return true;
    }
    return false;
}

// Is [vaddr, vaddr + length) inside the memory of one loaded segment?
static bool vaddr_in_image(const Elf64_Phdr *ph, uint16_t count, uint64_t vaddr, uint64_t length) {
    for (uint16_t i = 0; i < count; i++) {
        if (ph[i].p_type != PT_LOAD || vaddr < ph[i].p_vaddr) continue;
        uint64_t within = vaddr - ph[i].p_vaddr;
        if (within <= ph[i].p_memsz && length <= ph[i].p_memsz - within) return true;
    }
    return false;
}

#define DYN_CHUNK  16   // Dynamic entries read at a time
#define RELA_CHUNK 64   // Relocations read at a time

bool elf_apply_relocations_from(uint64_t *pml4, const elf_source_t *src, const Elf64_Ehdr *ehdr,
                                const Elf64_Phdr *ph, uint64_t bias, size_t *out_count) {
    if (out_count) *out_count = 0;
    size_t size = src->size;

    // The dynamic section is a list of (tag, value) pairs that says where
    // the relocation table is
    const Elf64_Phdr *dynamic = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dynamic = &ph[i]; break; }
    }
    if (!dynamic) return true; // Nothing to relocate

    if (!in_file(dynamic->p_offset, dynamic->p_filesz, size)) return false;
    size_t dyn_count = dynamic->p_filesz / sizeof(Elf64_Dyn);

    uint64_t rela_vaddr = 0, rela_size = 0, rela_entry = sizeof(Elf64_Rela);
    bool have_rela = false, ended = false;
    Elf64_Dyn dyn[DYN_CHUNK];
    for (size_t at = 0; at < dyn_count && !ended; at += DYN_CHUNK) {
        size_t n = dyn_count - at < DYN_CHUNK ? dyn_count - at : DYN_CHUNK;
        if (!elf_source_read(src, dynamic->p_offset + at * sizeof(Elf64_Dyn), n * sizeof(Elf64_Dyn), dyn)) return false;
        for (size_t i = 0; i < n && !ended; i++) {
            if (dyn[i].d_tag == DT_NULL) ended = true;
            else if (dyn[i].d_tag == DT_RELA) { rela_vaddr = dyn[i].d_val; have_rela = true; }
            else if (dyn[i].d_tag == DT_RELASZ) rela_size = dyn[i].d_val;
            else if (dyn[i].d_tag == DT_RELAENT) rela_entry = dyn[i].d_val;
        }
    }
    if (!have_rela || rela_size == 0) return true;

    if (rela_entry != sizeof(Elf64_Rela) || rela_size % sizeof(Elf64_Rela) != 0) return false;
    size_t count = rela_size / sizeof(Elf64_Rela);
    if (count > MAX_RELOCATIONS) return false;

    uint64_t table_offset;
    if (!vaddr_to_file_offset(ph, ehdr->e_phnum, size, rela_vaddr, rela_size, &table_offset)) return false;

    Elf64_Rela rela[RELA_CHUNK];
    for (size_t at = 0; at < count; at += RELA_CHUNK) {
        size_t n = count - at < RELA_CHUNK ? count - at : RELA_CHUNK;
        if (!elf_source_read(src, table_offset + at * sizeof(Elf64_Rela), n * sizeof(Elf64_Rela), rela)) return false;
        for (size_t i = 0; i < n; i++) {
            uint32_t type = (uint32_t)rela[i].r_info;
            if (type == R_X86_64_NONE) continue;
            if (type != R_X86_64_RELATIVE) return false; // Symbol lookups would need a dynamic linker

            // The place to patch must be inside what was loaded: otherwise a
            // crafted file could have the kernel write to any user address.
            // (A place in a segment left to be read later is not mapped yet,
            // and the write below fails: such a program is refused.)
            if (!vaddr_in_image(ph, ehdr->e_phnum, rela[i].r_offset, sizeof(uint64_t))) return false;

            uint64_t value = bias + (uint64_t)rela[i].r_addend;
            if (vmm_copy_to_space(pml4, bias + rela[i].r_offset, &value, sizeof(value)) != sizeof(value)) return false;
            if (out_count) (*out_count)++;
        }
    }
    return true;
}

bool elf_apply_relocations(uint64_t *pml4, const void *file, size_t size, uint64_t bias, size_t *out_count) {
    const Elf64_Ehdr *ehdr = file;
    elf_source_t src = { file, NULL, size };
    return elf_apply_relocations_from(pml4, &src, ehdr, (const Elf64_Phdr *)((const uint8_t *)file + ehdr->e_phoff),
                                      bias, out_count);
}
