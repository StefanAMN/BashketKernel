/**
 * @file elf.c
 * @brief ELF64 executable loader
 *
 * Loads a statically linked ELF64 executable into a user address space. The
 * file comes from disk or an initrd and is not trusted: every offset and size
 * in it is checked against the file size and against the user address range
 * before it is used.
 */

#include "elf.h"
#include "elf_dyn.h"
#include "random.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "string_util.h"

#define ELF_MAX_PHDRS      64
#define ELF_MAX_SEGMENT    (256ULL * 1024 * 1024) // Sanity limit per segment
#define ELF_MIN_VADDR      0x1000ULL              // Keep page 0 unmapped: NULL must fault

// True if [offset, offset + length) lies inside a file of `size` bytes.
static bool range_in_file(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

// `bias` is added to every address in the file: 0 for a fixed-address
// executable, the chosen load address for a position-independent one.
static bool load_segment(uint64_t *pml4, const uint8_t *file, size_t size, const Elf64_Phdr *ph, uint64_t bias) {
    if (ph->p_memsz == 0) return true;

    if (ph->p_filesz > ph->p_memsz) return false;
    if (!range_in_file(ph->p_offset, ph->p_filesz, size)) return false;
    if (ph->p_memsz > ELF_MAX_SEGMENT) return false;

    uint64_t vaddr = ph->p_vaddr + bias;
    if (vaddr < ph->p_vaddr) return false; // Wrapped around
    if (vaddr < ELF_MIN_VADDR) return false;
    if (vaddr >= USER_SPACE_END || ph->p_memsz > USER_SPACE_END - vaddr) return false;

    uint64_t flags = VMM_PRESENT | VMM_USER;
    if (ph->p_flags & PF_W) flags |= VMM_WRITABLE;
    if (!(ph->p_flags & PF_X)) flags |= VMM_NO_EXECUTE;

    uint64_t first_page = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = vaddr + ph->p_memsz;

    for (uint64_t page = first_page; page < end; page += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(pml4, page);
        if (pte & VMM_PRESENT) {
            // Two segments share this page. Keep the frame and widen the
            // permissions to cover both.
            uint64_t merged = (pte & (VMM_WRITABLE | VMM_USER)) | (flags & VMM_WRITABLE) | VMM_PRESENT | VMM_USER;
            if ((pte & VMM_NO_EXECUTE) && (flags & VMM_NO_EXECUTE)) merged |= VMM_NO_EXECUTE;
            if (!vmm_map_page(pml4, page, pte & PTE_ADDR_MASK, merged)) return false;
        } else if (!vmm_alloc_user_page(pml4, page, flags)) {
            return false; // Out of memory. Pages are zeroed on allocation, which covers .bss.
        }
    }

    // Copy through the direct map: works for read-only segments, and without
    // switching to the new address space.
    return vmm_copy_to_space(pml4, vaddr, file + ph->p_offset, ph->p_filesz) == ph->p_filesz;
}

// Where to put a position-independent executable: a random page-aligned
// address between 4 GiB and 260 GiB (26 bits of randomness). An attacker who
// can make a program jump somewhere first has to guess where anything is.
// The numbers come from the kernel's generator (random.h).
static uint64_t random_load_base(void) {
    return ELF_PIE_BASE_MIN + ((uint64_t)(get_random_u32() & ELF_PIE_RANDOM_MASK) << 12);
}

bool elf_load(uint64_t *pml4, const void *elf_data, size_t size, uint64_t *out_entry, uint64_t *out_image_end) {
    return elf_load_at(pml4, elf_data, size, 0, out_entry, out_image_end, NULL);
}

bool elf_load_at(uint64_t *pml4, const void *elf_data, size_t size, uint64_t requested_base,
                 uint64_t *out_entry, uint64_t *out_image_end, uint64_t *out_base) {
    if (!pml4 || !elf_data || !out_entry) return false;

    if (size < sizeof(Elf64_Ehdr)) {
        kprintf("ELF: file too small for an ELF header\n");
        return false;
    }

    const uint8_t *file = (const uint8_t *)elf_data;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        kprintf("ELF: bad magic\n");
        return false;
    }
    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB ||
        ehdr->e_machine != EM_X86_64 || (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)) {
        kprintf("ELF: not a little-endian x86_64 static executable\n");
        return false;
    }
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr) || ehdr->e_phnum == 0 || ehdr->e_phnum > ELF_MAX_PHDRS ||
        !range_in_file(ehdr->e_phoff, (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr), size)) {
        kprintf("ELF: program header table is outside the file\n");
        return false;
    }

    // A fixed-address executable goes where it says. A position-independent
    // one goes where we say: where the caller asked, or somewhere random.
    uint64_t bias = 0;
    if (ehdr->e_type == ET_DYN) {
        bias = requested_base ? requested_base : random_load_base();
        if (bias & (PAGE_SIZE - 1)) return false;
    }

    const Elf64_Phdr *phdr = (const Elf64_Phdr *)(file + ehdr->e_phoff);
    uint64_t image_end = 0;
    bool loaded_any = false;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        if (!load_segment(pml4, file, size, &phdr[i], bias)) {
            kprintf("ELF: segment %u is invalid or could not be mapped\n", i);
            return false;
        }
        loaded_any = true;

        uint64_t seg_end = phdr[i].p_vaddr + bias + phdr[i].p_memsz;
        if (seg_end > image_end) image_end = seg_end;
    }

    if (ehdr->e_type == ET_DYN && !elf_apply_relocations(pml4, elf_data, size, bias, NULL)) {
        kprintf("ELF: relocations are malformed or of an unsupported kind\n");
        return false;
    }

    // The entry point must land on a page we mapped as executable
    uint64_t entry = ehdr->e_entry + bias;
    uint64_t entry_pte = vmm_get_pte(pml4, entry & ~(uint64_t)(PAGE_SIZE - 1));
    if (!loaded_any || entry < ehdr->e_entry || entry >= USER_SPACE_END ||
        !(entry_pte & VMM_PRESENT) || (entry_pte & VMM_NO_EXECUTE)) {
        kprintf("ELF: entry point is not in an executable segment\n");
        return false;
    }

    *out_entry = entry;
    if (out_image_end) *out_image_end = image_end;
    if (out_base) *out_base = bias;
    return true;
}
