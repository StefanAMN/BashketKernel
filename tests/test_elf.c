#include "test.h"
#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "kstring.h"

// Load `image` into a throwaway address space and report whether the loader
// accepted it. Also checks that a rejected image leaves no frames behind.
static bool try_load(const void *image, size_t size) {
    size_t free_before = pmm_get_free_frames();

    uint64_t *space = vmm_create_address_space();
    if (!space) return false;

    uint64_t entry = 0;
    bool ok = elf_load(space, image, size, &entry, NULL);
    vmm_destroy_address_space(space);

    EXPECT_TRUE(pmm_get_free_frames() == free_before);
    return ok;
}

TEST(elf_loads_real_program) {
    vnode_t *file = vfs_find(NULL, "/bin/hello");
    EXPECT_TRUE(file != NULL);
    if (!file) return;

    uint64_t *space = vmm_create_address_space();
    uint64_t entry = 0, image_end = 0;
    EXPECT_TRUE(elf_load(space, file->data, file->size, &entry, &image_end));

    // User programs are linked at 4 MiB (user/linker.ld)
    EXPECT_TRUE(entry >= 0x400000 && entry < image_end);

    // Text is user-executable and read-only; nothing landed in kernel space
    uint64_t text_pte = vmm_get_pte(space, entry & ~0xFFFULL);
    EXPECT_TRUE((text_pte & VMM_PRESENT) && (text_pte & VMM_USER));
    EXPECT_TRUE(!(text_pte & VMM_WRITABLE));
    EXPECT_TRUE(!(text_pte & VMM_NO_EXECUTE));

    // The first bytes of the mapped text equal the bytes in the file
    const Elf64_Ehdr *ehdr = file->data;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)file->data + ehdr->e_phoff);
    const uint8_t *mapped = (const uint8_t *)(vmm_get_phys(space, ph[0].p_vaddr & ~0xFFFULL) +
                                               pmm_get_hhdm_offset() + (ph[0].p_vaddr & 0xFFF));
    EXPECT_EQ(memcmp(mapped, (const uint8_t *)file->data + ph[0].p_offset, 64), 0);

    vmm_destroy_address_space(space);
}

TEST(elf_rejects_malformed_files) {
    vnode_t *file = vfs_find(NULL, "/bin/hello");
    EXPECT_TRUE(file != NULL);
    if (!file) return;

    // Work on a private copy that each case corrupts in one way
    uint8_t *copy = kmalloc(file->size);
    EXPECT_TRUE(copy != NULL);
    if (!copy) return;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)copy;

#define FRESH_COPY() memcpy(copy, file->data, file->size)
#define FIRST_PHDR() ((Elf64_Phdr *)(copy + ehdr->e_phoff))

    FRESH_COPY();
    EXPECT_TRUE(try_load(copy, file->size));          // The untouched copy loads

    EXPECT_TRUE(!try_load(copy, 10));                 // Shorter than the header
    EXPECT_TRUE(!try_load(copy, sizeof(Elf64_Ehdr))); // Program headers cut off

    FRESH_COPY(); ehdr->e_ident[0] = 0;               // Bad magic
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); ehdr->e_ident[4] = 1;               // 32-bit class
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); ehdr->e_machine = 3;                // i386
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); ehdr->e_phoff = file->size + 4096;  // Header table past the end
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); ehdr->e_phoff = 0xFFFFFFFFFFFFFFF0ULL; // Offset that overflows
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); ehdr->e_phnum = 0xFFFF;             // Absurd header count
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); FIRST_PHDR()->p_offset = file->size; // Segment data outside file
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); FIRST_PHDR()->p_filesz = FIRST_PHDR()->p_memsz + 1; // More file than memory
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); FIRST_PHDR()->p_vaddr = 0xFFFFFFFF80000000ULL; // Aimed at the kernel
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); FIRST_PHDR()->p_vaddr = 0x00007FFFFFFFF000ULL; // Runs off user space
    FIRST_PHDR()->p_memsz = 0x10000;
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); FIRST_PHDR()->p_vaddr = 0;          // Would map the NULL page
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); FIRST_PHDR()->p_memsz = 1ULL << 40; // 1 TiB segment
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); ehdr->e_entry = 0xFFFFFFFF80000000ULL; // Entry in the kernel
    EXPECT_TRUE(!try_load(copy, file->size));

    FRESH_COPY(); ehdr->e_entry = 0x10000000;         // Entry in unmapped memory
    EXPECT_TRUE(!try_load(copy, file->size));

#undef FRESH_COPY
#undef FIRST_PHDR
    kfree(copy);
}

void test_suite_elf(void) {
    kprintf("Suite: ELF64 Executable Loader\n");
    RUN_TEST(elf_loads_real_program);
    RUN_TEST(elf_rejects_malformed_files);
}
