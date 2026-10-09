#include "test.h"
#include "elf.h"
#include "elf_dyn.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "kstring.h"

// Load `image` into a throwaway address space and report whether the loader
// accepted it. Also checks that a rejected image leaves no frames behind.
static bool try_load(const void *image, size_t size) {
    size_t free_before = test_free_frames();

    uint64_t *space = vmm_create_address_space();
    if (!space) return false;

    uint64_t entry = 0;
    bool ok = elf_load(space, image, size, &entry, NULL);
    vmm_destroy_address_space(space);

    EXPECT_TRUE(test_free_frames() == free_before);
    return ok;
}

#define TEST_BASE 0x200000000ULL   // Where the tests ask for programs to be placed

TEST(elf_loads_real_program) {
    vnode_t *file = vfs_find(NULL, "/bin/hello");
    EXPECT_TRUE(file != NULL);
    if (!file) return;

    const Elf64_Ehdr *ehdr = file->data;
    EXPECT_EQ(ehdr->e_type, ET_DYN);       // User programs are position-independent

    uint64_t *space = vmm_create_address_space();
    uint64_t entry = 0, image_end = 0, base = 0;
    EXPECT_TRUE(elf_load_at(space, file->data, file->size, TEST_BASE, &entry, &image_end, &base));
    EXPECT_TRUE(base == TEST_BASE);
    EXPECT_TRUE(entry == TEST_BASE + ehdr->e_entry && entry < image_end);
    EXPECT_TRUE(image_end > TEST_BASE && image_end < TEST_BASE + (16 << 20));

    // Text is user-executable and read-only
    uint64_t text_pte = vmm_get_pte(space, entry & ~0xFFFULL);
    EXPECT_TRUE((text_pte & VMM_PRESENT) && (text_pte & VMM_USER));
    EXPECT_TRUE(!(text_pte & VMM_WRITABLE));
    EXPECT_TRUE(!(text_pte & VMM_NO_EXECUTE));

    // Nothing was put where the file's own (zero-based) addresses point
    EXPECT_TRUE(vmm_get_phys(space, 0x1000) == 0);

    // The first bytes of the mapped text equal the bytes in the file
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)file->data + ehdr->e_phoff);
    uint64_t text_at = TEST_BASE + ph[0].p_vaddr;
    const uint8_t *mapped = (const uint8_t *)(vmm_get_phys(space, text_at & ~0xFFFULL) +
                                               pmm_get_hhdm_offset() + (text_at & 0xFFF));
    EXPECT_EQ(memcmp(mapped, (const uint8_t *)file->data + ph[0].p_offset, 64), 0);

    vmm_destroy_address_space(space);
}

// Read 8 bytes at a user address of another address space
static uint64_t peek64(uint64_t *space, uint64_t addr) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t phys = vmm_get_phys(space, (addr + (uint64_t)i) & ~0xFFFULL);
        if (!phys) return 0xDEADDEADDEADDEADULL;
        value |= (uint64_t)*(const uint8_t *)(phys + pmm_get_hhdm_offset() + ((addr + (uint64_t)i) & 0xFFF)) << (8 * i);
    }
    return value;
}

// Find the relocation table of a position-independent executable in its file
static const Elf64_Rela *find_relocations(const uint8_t *file, size_t *count, const Elf64_Dyn **out_dyn) {
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + ehdr->e_phoff);
    const Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn = (const Elf64_Dyn *)(file + ph[i].p_offset);
    }
    if (!dyn) return NULL;
    if (out_dyn) *out_dyn = dyn;

    uint64_t table = 0, size = 0;
    for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_RELA) table = dyn[i].d_val;
        if (dyn[i].d_tag == DT_RELASZ) size = dyn[i].d_val;
    }
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && table >= ph[i].p_vaddr && table < ph[i].p_vaddr + ph[i].p_filesz) {
            *count = size / sizeof(Elf64_Rela);
            return (const Elf64_Rela *)(file + ph[i].p_offset + (table - ph[i].p_vaddr));
        }
    }
    return NULL;
}

// /bin/colors has a table of pointers to strings: absolute addresses, which
// are only right once the loader has added the load address to each.
TEST(elf_applies_relocations) {
    vnode_t *file = vfs_find(NULL, "/bin/colors");
    EXPECT_TRUE(file != NULL);
    if (!file) return;

    size_t count = 0;
    const Elf64_Rela *rela = find_relocations(file->data, &count, NULL);
    EXPECT_TRUE(rela != NULL && count >= 8);
    if (!rela) return;

    // Loaded at two different places, every relocated word is that place
    // plus the same constant
    static const uint64_t bases[2] = { TEST_BASE, 0x7654321000ULL };
    for (int b = 0; b < 2; b++) {
        uint64_t *space = vmm_create_address_space();
        uint64_t entry = 0;
        EXPECT_TRUE(elf_load_at(space, file->data, file->size, bases[b], &entry, NULL, NULL));
        int all_correct = 1;
        for (size_t i = 0; i < count; i++) {
            if ((uint32_t)rela[i].r_info != R_X86_64_RELATIVE) continue;
            uint64_t stored = peek64(space, bases[b] + rela[i].r_offset);
            all_correct &= (stored == bases[b] + (uint64_t)rela[i].r_addend);
        }
        EXPECT_TRUE(all_correct);
        vmm_destroy_address_space(space);
    }

    // The count reported agrees with the file
    uint64_t *space = vmm_create_address_space();
    uint64_t entry = 0;
    size_t applied = 0;
    EXPECT_TRUE(elf_load_at(space, file->data, file->size, TEST_BASE, &entry, NULL, NULL));
    EXPECT_TRUE(elf_apply_relocations(space, file->data, file->size, TEST_BASE, &applied));
    EXPECT_TRUE(applied == count);
    vmm_destroy_address_space(space);
}

TEST(elf_load_address_is_random) {
    vnode_t *file = vfs_find(NULL, "/bin/hello");
    if (!file) return;

    // Left to itself the loader picks a different page-aligned address each
    // time, inside its documented range
    uint64_t seen[8];
    int in_range = 1, distinct = 1;
    for (int i = 0; i < 8; i++) {
        uint64_t *space = vmm_create_address_space();
        uint64_t entry = 0, base = 0;
        EXPECT_TRUE(elf_load_at(space, file->data, file->size, 0, &entry, NULL, &base));
        vmm_destroy_address_space(space);

        seen[i] = base;
        in_range &= (base >= ELF_PIE_BASE_MIN) && ((base & 0xFFF) == 0) &&
                    (base < ELF_PIE_BASE_MIN + ((uint64_t)(ELF_PIE_RANDOM_MASK + 1) << 12));
        for (int j = 0; j < i; j++) distinct &= (seen[j] != base);
    }
    EXPECT_TRUE(in_range);
    EXPECT_TRUE(distinct);   // Eight draws from 2^26: a repeat would mean it is not random
}

TEST(elf_rejects_bad_placement_and_relocations) {
    vnode_t *file = vfs_find(NULL, "/bin/colors");
    if (!file) return;
    size_t free_before = test_free_frames();

    uint8_t *copy = kmalloc(file->size);
    EXPECT_TRUE(copy != NULL);
    if (!copy) return;

#define FRESH() memcpy(copy, file->data, file->size)
#define LOADS_AT(base) ({ uint64_t *sp = vmm_create_address_space(); uint64_t e = 0; \
                          bool r = elf_load_at(sp, copy, file->size, (base), &e, NULL, NULL); \
                          vmm_destroy_address_space(sp); r; })

    FRESH();
    EXPECT_TRUE(LOADS_AT(TEST_BASE));
    EXPECT_TRUE(!LOADS_AT(TEST_BASE + 0x123));               // Not page aligned
    EXPECT_TRUE(!LOADS_AT(0x00007FFFFFFFF000ULL));           // Runs off the end of user space
    EXPECT_TRUE(!LOADS_AT(0xFFFFFFFF80000000ULL));           // In the kernel
    EXPECT_TRUE(!LOADS_AT(0xFFFFFFFFFFFFF000ULL));           // Wraps around

    size_t count = 0;
    const Elf64_Dyn *dyn_const = NULL;
    Elf64_Rela *rela = (Elf64_Rela *)find_relocations(copy, &count, &dyn_const);
    Elf64_Dyn *dyn = (Elf64_Dyn *)dyn_const;
    EXPECT_TRUE(rela != NULL && dyn != NULL && count > 0);

    if (rela && dyn) {
        // A relocation that would write outside the loaded image: this is
        // how a crafted file would try to make the kernel patch memory of
        // its choosing
        FRESH(); rela[0].r_offset = 0x40000000;
        EXPECT_TRUE(!LOADS_AT(TEST_BASE));
        FRESH(); rela[0].r_offset = 0xFFFFFFFF80000000ULL - TEST_BASE;   // Lands in the kernel after the bias
        EXPECT_TRUE(!LOADS_AT(TEST_BASE));
        FRESH(); rela[count - 1].r_offset = ~0ULL;
        EXPECT_TRUE(!LOADS_AT(TEST_BASE));

        // A kind of relocation that needs a symbol table and a dynamic linker
        FRESH(); rela[0].r_info = 6;                         // R_X86_64_GLOB_DAT
        EXPECT_TRUE(!LOADS_AT(TEST_BASE));

        // The table itself described wrongly
        for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
            if (dyn[i].d_tag == DT_RELASZ) {
                FRESH(); dyn[i].d_val = 1ULL << 40;          // Far larger than the file
                EXPECT_TRUE(!LOADS_AT(TEST_BASE));
                FRESH(); dyn[i].d_val += 5;                  // Not a whole number of entries
                EXPECT_TRUE(!LOADS_AT(TEST_BASE));
            } else if (dyn[i].d_tag == DT_RELAENT) {
                FRESH(); dyn[i].d_val = 16;                  // Wrong entry size
                EXPECT_TRUE(!LOADS_AT(TEST_BASE));
            } else if (dyn[i].d_tag == DT_RELA) {
                FRESH(); dyn[i].d_val = 0x70000000;          // Table not inside any segment
                EXPECT_TRUE(!LOADS_AT(TEST_BASE));
            }
        }

        // The dynamic section placed outside the file
        Elf64_Ehdr *ehdr = (Elf64_Ehdr *)copy;
        Elf64_Phdr *ph = (Elf64_Phdr *)(copy + ehdr->e_phoff);
        FRESH();
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (ph[i].p_type == PT_DYNAMIC) ph[i].p_offset = file->size + 4096;
        }
        EXPECT_TRUE(!LOADS_AT(TEST_BASE));
    }

    // A relocation of type "none" is skipped, and the untouched file still loads
    FRESH();
    if (rela) rela[0].r_info = R_X86_64_NONE;
    EXPECT_TRUE(LOADS_AT(TEST_BASE));
    FRESH();
    EXPECT_TRUE(LOADS_AT(TEST_BASE));

#undef FRESH
#undef LOADS_AT
    kfree(copy);
    EXPECT_TRUE(test_free_frames() == free_before);      // Rejected images left nothing behind
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

    FRESH_COPY(); ehdr->e_type = ET_EXEC;             // As a fixed-address program it would map page 0
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
    RUN_TEST(elf_applies_relocations);
    RUN_TEST(elf_load_address_is_random);
    RUN_TEST(elf_rejects_bad_placement_and_relocations);
    RUN_TEST(elf_rejects_malformed_files);
}
