/**
 * @file elf_dyn.c
 * @brief Relocation of position-independent executables (see elf_dyn.h)
 */

#include "elf_dyn.h"
#include "vmm.h"
#include "pmm.h"

#define MAX_RELOCATIONS (1u << 20)

static bool in_file(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

// Program headers were validated by the loader before this is called
static const Elf64_Phdr *program_headers(const void *file) {
    const Elf64_Ehdr *ehdr = file;
    return (const Elf64_Phdr *)((const uint8_t *)file + ehdr->e_phoff);
}

// Where in the file is the data for link-time address `vaddr`? Returns false
// unless [vaddr, vaddr + length) lies inside the file-backed part of one
// loaded segment.
static bool vaddr_to_file_offset(const void *file, size_t size, uint64_t vaddr, uint64_t length, uint64_t *out_offset) {
    const Elf64_Ehdr *ehdr = file;
    const Elf64_Phdr *ph = program_headers(file);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
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
static bool vaddr_in_image(const void *file, uint64_t vaddr, uint64_t length) {
    const Elf64_Ehdr *ehdr = file;
    const Elf64_Phdr *ph = program_headers(file);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || vaddr < ph[i].p_vaddr) continue;
        uint64_t within = vaddr - ph[i].p_vaddr;
        if (within <= ph[i].p_memsz && length <= ph[i].p_memsz - within) return true;
    }
    return false;
}

bool elf_apply_relocations(uint64_t *pml4, const void *file, size_t size, uint64_t bias, size_t *out_count) {
    const Elf64_Ehdr *ehdr = file;
    const Elf64_Phdr *ph = program_headers(file);
    if (out_count) *out_count = 0;

    // The dynamic section is a list of (tag, value) pairs that says where
    // the relocation table is
    const Elf64_Phdr *dynamic = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dynamic = &ph[i]; break; }
    }
    if (!dynamic) return true; // Nothing to relocate

    if (!in_file(dynamic->p_offset, dynamic->p_filesz, size)) return false;
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)((const uint8_t *)file + dynamic->p_offset);
    size_t dyn_count = dynamic->p_filesz / sizeof(Elf64_Dyn);

    uint64_t rela_vaddr = 0, rela_size = 0, rela_entry = sizeof(Elf64_Rela);
    bool have_rela = false;
    for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_RELA) { rela_vaddr = dyn[i].d_val; have_rela = true; }
        else if (dyn[i].d_tag == DT_RELASZ) rela_size = dyn[i].d_val;
        else if (dyn[i].d_tag == DT_RELAENT) rela_entry = dyn[i].d_val;
    }
    if (!have_rela || rela_size == 0) return true;

    if (rela_entry != sizeof(Elf64_Rela) || rela_size % sizeof(Elf64_Rela) != 0) return false;
    size_t count = rela_size / sizeof(Elf64_Rela);
    if (count > MAX_RELOCATIONS) return false;

    uint64_t table_offset;
    if (!vaddr_to_file_offset(file, size, rela_vaddr, rela_size, &table_offset)) return false;
    const Elf64_Rela *rela = (const Elf64_Rela *)((const uint8_t *)file + table_offset);

    for (size_t i = 0; i < count; i++) {
        uint32_t type = (uint32_t)rela[i].r_info;
        if (type == R_X86_64_NONE) continue;
        if (type != R_X86_64_RELATIVE) return false; // Symbol lookups would need a dynamic linker

        // The place to patch must be inside what was loaded: otherwise a
        // crafted file could have the kernel write to any user address
        if (!vaddr_in_image(file, rela[i].r_offset, sizeof(uint64_t))) return false;

        uint64_t value = bias + (uint64_t)rela[i].r_addend;
        if (vmm_copy_to_space(pml4, bias + rela[i].r_offset, &value, sizeof(value)) != sizeof(value)) return false;
        if (out_count) (*out_count)++;
    }
    return true;
}
