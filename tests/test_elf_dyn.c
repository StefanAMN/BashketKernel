#include "test.h"
#include "elf_dyn.h"

TEST(elf_dynamic_relocation_test) {
    uint64_t location = 0x1000;
    EXPECT_TRUE(elf_relocate_symbol(R_X86_64_RELATIVE, &location, 0x400000) == 0);
    EXPECT_TRUE(location == 0x401000);

    uint64_t symbol_loc = 0;
    EXPECT_TRUE(elf_relocate_symbol(R_X86_64_JUMP_SLOT, &symbol_loc, 0x7FFFFFFF) == 0);
    EXPECT_TRUE(symbol_loc == 0x7FFFFFFF);
}

void test_suite_elf_dyn(void) {
    kprintf("Suite: Phase 28 ELF Dynamic Relocation Loader\n");
    RUN_TEST(elf_dynamic_relocation_test);
}
