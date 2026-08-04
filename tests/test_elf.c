#include "test.h"
#include "elf.h"

TEST(elf_validation) {
    // Create a mock buffer with non-ELF magic
    uint8_t dummy_buf[64] = {0};
    void *entry = NULL;

    bool valid = elf_load(dummy_buf, &entry);
    EXPECT_TRUE(!valid);

    // Mock ELF header with correct magic
    Elf64_Ehdr mock_hdr;
    mock_hdr.e_ident[0] = 0x7F;
    mock_hdr.e_ident[1] = 'E';
    mock_hdr.e_ident[2] = 'L';
    mock_hdr.e_ident[3] = 'F';
    mock_hdr.e_ident[4] = 2; // 64-bit
    mock_hdr.e_type = 2;     // Executable
    mock_hdr.e_entry = 0x400000;
    mock_hdr.e_phnum = 0;
    mock_hdr.e_phoff = 0;

    valid = elf_load(&mock_hdr, &entry);
    EXPECT_TRUE(valid);
    EXPECT_EQ((uint64_t)entry, 0x400000);
}

void test_suite_elf(void) {
    kprintf("Suite: Phase 5 ELF64 Executable Loader\n");
    RUN_TEST(elf_validation);
}
