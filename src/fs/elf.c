/**
 * @file elf.c
 * @brief ELF64 executable loader
 *
 * Loads a statically linked ELF64 executable into a user address space. The
 * file comes from disk or an initrd and is not trusted: every offset and size
 * in it is checked against the file size and against the user address range
 * before it is used.
 *
 * The file is read only where the loader needs it: the headers, the
 * writable segment (copied and relocated here), and the relocation table.
 * Read-only segments of a program loaded from a file (elf_load_program
 * with a node) are left for the caller to map from the file, so a page of
 * code is read when it is first run (see elf.h).
 */

#include "elf.h"
#include "mmap.h"
#include "elf_dyn.h"
#include "random.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "kprintf.h"
#include "kstring.h"

#define ELF_MAX_PHDRS      64
#define ELF_MAX_SEGMENT    (256ULL * 1024 * 1024) // Sanity limit per segment
#define ELF_MIN_VADDR      0x1000ULL              // Keep page 0 unmapped: NULL must fault

// True if [offset, offset + length) lies inside a file of `size` bytes.
static bool range_in_file(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

bool elf_source_read(const elf_source_t *src, uint64_t offset, uint64_t len, void *out) {
    if (!src || !out || !range_in_file(offset, len, src->size)) return false;
    if (len == 0) return true;
    if (src->data) {
        memcpy(out, src->data + offset, len);
        return true;
    }
    // A file with its own operations (a disk): read without the tree lock,
    // which the caller may hold (vfs_read takes it only for files in memory)
    return src->node && vfs_read(src->node, offset, len, out) == len;
}

// Copy `len` bytes of the file from `offset` to `vaddr` in `pml4`, whose
// pages are already mapped: straight from memory, or through a bounce
// buffer a page at a time from a disk file
static bool copy_from_source(uint64_t *pml4, const elf_source_t *src, uint64_t offset, uint64_t vaddr, uint64_t len) {
    if (src->data) {
        if (!range_in_file(offset, len, src->size)) return false;
        return vmm_copy_to_space(pml4, vaddr, src->data + offset, len) == len;
    }
    uint8_t *bounce = kmalloc(PAGE_SIZE);
    if (!bounce) return false;
    bool ok = true;
    for (uint64_t done = 0; ok && done < len; ) {
        uint64_t chunk = len - done < PAGE_SIZE ? len - done : PAGE_SIZE;
        ok = elf_source_read(src, offset + done, chunk, bounce) &&
             vmm_copy_to_space(pml4, vaddr + done, bounce, chunk) == chunk;
        done += chunk;
    }
    kfree(bounce);
    return ok;
}

// Check a loadable segment against the file and the user address range,
// given the load `bias` (0 for a fixed-address executable, the chosen load
// address for a position-independent one). True if it may be loaded.
static bool segment_valid(const Elf64_Phdr *ph, size_t size, uint64_t bias) {
    if (ph->p_memsz == 0) return true;
    if (ph->p_filesz > ph->p_memsz) return false;
    if (!range_in_file(ph->p_offset, ph->p_filesz, size)) return false;
    if (ph->p_memsz > ELF_MAX_SEGMENT) return false;

    uint64_t vaddr = ph->p_vaddr + bias;
    if (vaddr < ph->p_vaddr) return false; // Wrapped around
    if (vaddr < ELF_MIN_VADDR) return false;
    if (vaddr >= USER_SPACE_END || ph->p_memsz > USER_SPACE_END - vaddr) return false;
    return true;
}

// Map the pages of a (validated) segment and copy its file contents in.
static bool load_segment(uint64_t *pml4, const elf_source_t *src, const Elf64_Phdr *ph, uint64_t bias) {
    if (ph->p_memsz == 0) return true;
    uint64_t vaddr = ph->p_vaddr + bias;

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

    // Copied through the direct map: works for read-only segments, and
    // without switching to the new address space
    return copy_from_source(pml4, src, ph->p_offset, vaddr, ph->p_filesz);
}

// The page-aligned memory [start, end) of a (validated, non-empty) segment
static void segment_pages(const Elf64_Phdr *ph, uint64_t bias, uint64_t *start, uint64_t *end) {
    uint64_t vaddr = ph->p_vaddr + bias;
    *start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
    *end = page_align_up(vaddr + ph->p_memsz);
}

// May segment `which` be left to be read from the file page by page? Only
// a read-only one whose memory is all file contents (a zero-filled tail
// would show the file's next bytes instead), whose offset in the file and
// address agree within a page (a page of memory is then a page of the
// file), and which shares no page with another segment (that page would
// need both).
static bool segment_lazy(const Elf64_Phdr *phdr, uint16_t count, uint16_t which, uint64_t bias) {
    const Elf64_Phdr *ph = &phdr[which];
    if (ph->p_memsz == 0 || (ph->p_flags & PF_W) || ph->p_filesz != ph->p_memsz) return false;
    if ((ph->p_offset & (PAGE_SIZE - 1)) != ((ph->p_vaddr + bias) & (PAGE_SIZE - 1))) return false;

    uint64_t start, end;
    segment_pages(ph, bias, &start, &end);
    for (uint16_t i = 0; i < count; i++) {
        if (i == which || phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) continue;
        uint64_t other_start, other_end;
        segment_pages(&phdr[i], bias, &other_start, &other_end);
        if (start < other_end && other_start < end) return false;
    }
    return true;
}

// Where to put a position-independent executable: a random page-aligned
// address between 4 GiB and 260 GiB (26 bits of randomness). An attacker who
// can make a program jump somewhere first has to guess where anything is.
// The numbers come from the kernel's generator (random.h).
static uint64_t random_load_base(void) {
    return ELF_PIE_BASE_MIN + ((uint64_t)(get_random_u32() & ELF_PIE_RANDOM_MASK) << 12);
}

static bool load_image(uint64_t *pml4, const elf_source_t *src, uint64_t requested_base,
                       uint64_t *out_base, elf_program_t *out);

// elf_load_at and friends: the whole file in memory, everything loaded
static bool load_buffer(uint64_t *pml4, const void *elf_data, size_t size, uint64_t requested_base,
                        uint64_t *out_entry, uint64_t *out_image_end, uint64_t *out_base, elf_program_t *out) {
    if (!elf_data || !out_entry) return false;
    elf_source_t src = { elf_data, NULL, size };
    if (!load_image(pml4, &src, requested_base, out_base, out)) return false;
    *out_entry = out->entry;
    if (out_image_end) *out_image_end = out->image_end;
    return true;
}

bool elf_load(uint64_t *pml4, const void *elf_data, size_t size, uint64_t *out_entry, uint64_t *out_image_end) {
    elf_program_t program;
    return load_buffer(pml4, elf_data, size, 0, out_entry, out_image_end, NULL, &program);
}

bool elf_load_at(uint64_t *pml4, const void *elf_data, size_t size, uint64_t requested_base,
                 uint64_t *out_entry, uint64_t *out_image_end, uint64_t *out_base) {
    elf_program_t program;
    return load_buffer(pml4, elf_data, size, requested_base, out_entry, out_image_end, out_base, &program);
}

bool elf_load_program(uint64_t *pml4, const elf_source_t *src, elf_program_t *out) {
    return load_image(pml4, src, 0, NULL, out);
}

// The loader proper: check the headers, load (or, from a file, list for
// later) every segment, relocate, check the entry point. `requested_base`
// as for elf_load_at; *out_base receives the bias if not NULL.
static bool load_image(uint64_t *pml4, const elf_source_t *src, uint64_t requested_base,
                       uint64_t *out_base, elf_program_t *out) {
    if (!pml4 || !src || !out) return false;
    memset(out, 0, sizeof(*out));
    size_t size = src->size;

    Elf64_Ehdr header;
    if (size < sizeof(Elf64_Ehdr) || !elf_source_read(src, 0, sizeof(header), &header)) {
        kprintf("ELF: file too small for an ELF header\n");
        return false;
    }
    const Elf64_Ehdr *ehdr = &header;

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

    // A copy of the program headers: the file may not be in memory
    size_t phdr_bytes = (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdr = kmalloc(phdr_bytes);
    if (!phdr) return false;
    if (!elf_source_read(src, ehdr->e_phoff, phdr_bytes, phdr)) {
        kfree(phdr);
        return false;
    }

    uint64_t image_end = 0;
    bool loaded_any = false, ok = true;

    for (uint16_t i = 0; ok && i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        if (!segment_valid(&phdr[i], size, bias)) {
            kprintf("ELF: segment %u is invalid or could not be mapped\n", i);
            ok = false;
            break;
        }
        if (src->node && out->lazy_count < ELF_LAZY_SEGMENTS && segment_lazy(phdr, ehdr->e_phnum, i, bias)) {
            // Left for the caller to map from the file
            elf_lazy_t *lazy = &out->lazy[out->lazy_count++];
            segment_pages(&phdr[i], bias, &lazy->start, &lazy->end);
            lazy->file_offset = phdr[i].p_offset & ~(uint64_t)(PAGE_SIZE - 1);
            lazy->prot = PROT_READ | ((phdr[i].p_flags & PF_X) ? PROT_EXEC : 0);
        } else if (!load_segment(pml4, src, &phdr[i], bias)) {
            kprintf("ELF: segment %u is invalid or could not be mapped\n", i);
            ok = false;
            break;
        }
        loaded_any = true;

        uint64_t seg_end = phdr[i].p_vaddr + bias + phdr[i].p_memsz;
        if (seg_end > image_end) image_end = seg_end;
        if (out->segment_count < ELF_REPORTED_SEGMENTS && phdr[i].p_memsz > 0) {
            elf_segment_t *seg = &out->segments[out->segment_count++];
            segment_pages(&phdr[i], bias, &seg->start, &seg->end);
            seg->prot = PROT_READ | ((phdr[i].p_flags & PF_W) ? PROT_WRITE : 0) | ((phdr[i].p_flags & PF_X) ? PROT_EXEC : 0);
        }
    }

    if (ok && ehdr->e_type == ET_DYN && !elf_apply_relocations_from(pml4, src, ehdr, phdr, bias, NULL)) {
        kprintf("ELF: relocations are malformed or of an unsupported kind\n");
        ok = false;
    }

    // The entry point must lie in an executable segment (which may not be
    // loaded yet: a page of code is read when first run)
    uint64_t entry = ehdr->e_entry + bias;
    bool entry_ok = false;
    for (uint16_t i = 0; ok && i < ehdr->e_phnum && !entry_ok; i++) {
        if (phdr[i].p_type != PT_LOAD || !(phdr[i].p_flags & PF_X)) continue;
        entry_ok = ehdr->e_entry >= phdr[i].p_vaddr && ehdr->e_entry - phdr[i].p_vaddr < phdr[i].p_memsz;
    }
    kfree(phdr);
    if (!ok) return false;
    if (!loaded_any || entry < ehdr->e_entry || entry >= USER_SPACE_END || !entry_ok) {
        kprintf("ELF: entry point is not in an executable segment\n");
        return false;
    }

    out->entry = entry;
    out->image_end = image_end;
    if (out_base) *out_base = bias;
    return true;
}
