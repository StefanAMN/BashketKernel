#include "../src/test.h"
#include "../src/idt.h"
#include "../src/gdt.h"

static uint64_t gate_handler_addr(int vector) {
    struct idt_entry *e = &idt_entries[vector];
    return (uint64_t)e->offset_low
         | ((uint64_t)e->offset_mid  << 16)
         | ((uint64_t)e->offset_high << 32);
}

TEST(sample_gates_point_at_their_stub) {
    int vectors[] = {0, 8, 14, 31};
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        int v = vectors[i];
        EXPECT_EQ(idt_entries[v].selector, KERNEL_CODE_SEL);
        EXPECT_EQ(idt_entries[v].type_attr, IDT_TYPE_INTERRUPT_GATE);
        EXPECT_TRUE(gate_handler_addr(v) == (uint64_t)isr_stub_table[v]);
    }
}

TEST(unpopulated_vectors_are_not_present) {
    // Vectors 32-255 have no handler yet -- Present bit (0x80) must be clear.
    EXPECT_TRUE((idt_entries[32].type_attr & 0x80) == 0);
    EXPECT_TRUE((idt_entries[255].type_attr & 0x80) == 0);
}

TEST(stub_table_entries_are_distinct) {
    // Catches a macro-expansion mistake that produced 32 copies of one label.
    bool all_nonnull = true;
    bool all_distinct = true;
    for (int i = 0; i < IDT_EXCEPTION_COUNT; i++) {
        if (isr_stub_table[i] == 0) all_nonnull = false;
        for (int j = i + 1; j < IDT_EXCEPTION_COUNT; j++) {
            if (isr_stub_table[i] == isr_stub_table[j]) all_distinct = false;
        }
    }
    EXPECT_TRUE(all_nonnull);
    EXPECT_TRUE(all_distinct);
}

void test_suite_idt(void) {
    kprintf("Suite: IDT\n");
    RUN_TEST(sample_gates_point_at_their_stub);
    RUN_TEST(unpopulated_vectors_are_not_present);
    RUN_TEST(stub_table_entries_are_distinct);
}
