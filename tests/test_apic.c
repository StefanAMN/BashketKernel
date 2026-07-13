#include "test.h"
#include "lapic.h"
#include "ioapic.h"

TEST(apic_initialization_test) {
    uint32_t id = lapic_get_id();
    EXPECT_TRUE(id == 0); // Primary CPU APIC ID 0
}

void test_suite_apic(void) {
    kprintf("Suite: Phase 9 APIC & IOAPIC Drivers\n");
    RUN_TEST(apic_initialization_test);
}
