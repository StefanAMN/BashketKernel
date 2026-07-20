/**
 * @file acpi.c
 * @brief Minimal ACPI table walker (RSDP -> RSDT/XSDT -> MADT)
 *
 * Structure layouts follow the ACPI specification, sections 5.2.5 (RSDP),
 * 5.2.6 (table header), 5.2.7/5.2.8 (RSDT/XSDT) and 5.2.12 (MADT).
 */

#include "acpi.h"
#include "pmm.h"
#include "kprintf.h"
#include <stddef.h>

struct acpi_rsdp {
    char     signature[8];   // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       // 0 = ACPI 1.0 (RSDT only), 2+ = XSDT available
    uint32_t rsdt_address;
    // Fields below exist only when revision >= 2
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;         // Whole table, header included
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;
    // Followed by variable-length entries: { uint8_t type; uint8_t length; ... }
} __attribute__((packed));

#define MADT_ENTRY_INT_SOURCE_OVERRIDE 2

struct madt_int_source_override {
    uint8_t  type;           // 2
    uint8_t  length;         // 10
    uint8_t  bus;            // 0 = ISA
    uint8_t  source;         // ISA IRQ number
    uint32_t gsi;            // Global System Interrupt it is wired to
    uint16_t flags;          // Polarity and trigger mode
} __attribute__((packed));

struct isa_override {
    bool     present;
    uint32_t gsi;
    uint16_t flags;
};

static struct isa_override isa_overrides[16];

// ACPI tables hold physical addresses; reach them through the direct map.
static void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + pmm_get_hhdm_offset());
}

static bool signature_is(const char *sig, const char *want) {
    return sig[0] == want[0] && sig[1] == want[1] && sig[2] == want[2] && sig[3] == want[3];
}

static void parse_madt(const struct acpi_madt *madt) {
    const uint8_t *ptr = (const uint8_t *)madt + sizeof(struct acpi_madt);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (ptr + 2 <= end) {
        uint8_t type = ptr[0];
        uint8_t length = ptr[1];
        if (length < 2 || ptr + length > end) break; // Malformed entry

        if (type == MADT_ENTRY_INT_SOURCE_OVERRIDE &&
            length >= sizeof(struct madt_int_source_override)) {
            const struct madt_int_source_override *iso = (const void *)ptr;
            if (iso->bus == 0 && iso->source < 16) {
                isa_overrides[iso->source].present = true;
                isa_overrides[iso->source].gsi = iso->gsi;
                isa_overrides[iso->source].flags = iso->flags;
                kprintf("ACPI: ISA IRQ %u is wired to GSI %u (flags 0x%x)\n",
                        iso->source, iso->gsi, iso->flags);
            }
        }

        ptr += length;
    }
}

bool acpi_init(void *rsdp_ptr) {
    if (!rsdp_ptr) {
        kprintf("ACPI: No RSDP from bootloader; assuming identity ISA IRQ routing\n");
        return false;
    }

    const struct acpi_rsdp *rsdp = rsdp_ptr;

    // Prefer the XSDT (64-bit pointers) when the firmware provides one
    bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const struct acpi_sdt_header *root =
        phys_to_virt(use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address);
    size_t entry_size = use_xsdt ? 8 : 4;
    size_t entry_count = (root->length - sizeof(struct acpi_sdt_header)) / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(struct acpi_sdt_header);

    for (size_t i = 0; i < entry_count; i++) {
        uint64_t table_phys = use_xsdt
            ? *(const uint64_t *)(entries + i * 8)
            : *(const uint32_t *)(entries + i * 4);
        const struct acpi_sdt_header *table = phys_to_virt(table_phys);

        if (signature_is(table->signature, "APIC")) {
            parse_madt((const struct acpi_madt *)table);
            return true;
        }
    }

    kprintf("ACPI: MADT not found; assuming identity ISA IRQ routing\n");
    return false;
}

uint32_t acpi_isa_irq_to_gsi(uint8_t irq, uint16_t *out_flags) {
    if (irq < 16 && isa_overrides[irq].present) {
        if (out_flags) *out_flags = isa_overrides[irq].flags;
        return isa_overrides[irq].gsi;
    }
    if (out_flags) *out_flags = 0;
    return irq;
}
