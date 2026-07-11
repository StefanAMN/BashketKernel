/**
 * @file elf.c
 * @brief Implementation of ELF64 Binary Parser and Memory Mapper
 */

#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "string_util.h"

bool elf_load(const void *elf_data, void **out_entry_point) {
    if (!elf_data || !out_entry_point) return false;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;

    // Verify 0x7F 'E' 'L' 'F' magic header
    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC) {
        kprintf("ELF ERROR: Invalid ELF magic bytes!\n");
        return false;
    }

    // Verify 64-bit format
    if (ehdr->e_ident[4] != 2) { // 2 = 64-bit
        kprintf("ELF ERROR: Only 64-bit ELF binaries supported!\n");
        return false;
    }

    const uint8_t *file_bytes = (const uint8_t *)elf_data;
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)(file_bytes + ehdr->e_phoff);

    // Iterate program headers and map PT_LOAD segments
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            uint64_t vaddr = phdr[i].p_vaddr;
            uint64_t memsz = phdr[i].p_memsz;
            uint64_t filesz = phdr[i].p_filesz;
            uint64_t offset = phdr[i].p_offset;

            size_t page_count = (memsz + PAGE_SIZE - 1) / PAGE_SIZE;

            uint64_t flags = VMM_PRESENT | VMM_USER;
            if (phdr[i].p_flags & PF_W) {
                flags |= VMM_WRITABLE;
            }

            for (size_t p = 0; p < page_count; p++) {
                uint64_t page_vaddr = (vaddr & PTE_ADDR_MASK) + (p * PAGE_SIZE);
                void *phys_frame = pmm_alloc_frame();
                if (!phys_frame) {
                    kprintf("ELF ERROR: Out of physical memory mapping ELF segment!\n");
                    return false;
                }

                vmm_map_page(NULL, page_vaddr, (uint64_t)phys_frame, flags);
            }

            // Copy segment data into mapped virtual memory
            if (filesz > 0) {
                uint8_t *dest = (uint8_t *)vaddr;
                memset(dest, 0, memsz);
                memcpy(dest, file_bytes + offset, filesz);
            }
        }
    }

    *out_entry_point = (void *)ehdr->e_entry;
    kprintf("ELF Loader: Binary loaded successfully (Entry Point: %p)\n", *out_entry_point);
    return true;
}
