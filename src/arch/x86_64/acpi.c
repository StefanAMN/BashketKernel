/**
 * @file acpi.c
 * @brief Minimal ACPI table walker (RSDP -> RSDT/XSDT -> MADT, FADT -> DSDT)
 *
 * Structure layouts follow the ACPI specification, sections 5.2.5 (RSDP),
 * 5.2.6 (table header), 5.2.7/5.2.8 (RSDT/XSDT) and 5.2.12 (MADT).
 */

#include "acpi.h"
#include "pmm.h"
#include "kprintf.h"
#include "io.h"
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

#define MADT_ENTRY_IOAPIC              1
#define MADT_ENTRY_INT_SOURCE_OVERRIDE 2

struct madt_ioapic {
    uint8_t  type;           // 1
    uint8_t  length;         // 12
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t address;        // Physical address of its registers
    uint32_t gsi_base;       // The first Global System Interrupt it handles
} __attribute__((packed));

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

// The first I/O APIC the MADT lists (the only one this kernel drives)
static bool ioapic_found;
static uint32_t ioapic_address, ioapic_gsi_base;

// ACPI tables hold physical addresses (firmware memory, which the
// bootloader's direct map covers); reach them through the direct map.
static void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + pmm_get_hhdm_offset());
}

// Whether the 4-character table signature `sig` is `want`
static bool signature_is(const char *sig, const char *want) {
    return sig[0] == want[0] && sig[1] == want[1] && sig[2] == want[2] && sig[3] == want[3];
}

// Walk the MADT's entries and record the first I/O APIC and every ISA
// interrupt source override. Entries are bounds-checked against the table's
// length (the firmware's bytes are not trusted); a malformed one ends the walk.
static void parse_madt(const struct acpi_madt *madt) {
    const uint8_t *ptr = (const uint8_t *)madt + sizeof(struct acpi_madt);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (ptr + 2 <= end) {
        uint8_t type = ptr[0];
        uint8_t length = ptr[1];
        if (length < 2 || ptr + length > end) break; // Malformed entry

        if (type == MADT_ENTRY_IOAPIC && length >= sizeof(struct madt_ioapic) && !ioapic_found) {
            const struct madt_ioapic *io = (const void *)ptr;
            ioapic_found = true;
            ioapic_address = io->address;
            ioapic_gsi_base = io->gsi_base;
        } else if (type == MADT_ENTRY_INT_SOURCE_OVERRIDE &&
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

// --- Power: the FADT and the \_S5 object -------------------------------------
//
// Switching a PC off is a write to the "PM1 control" register: a sleep type
// in bits 10-12 and the sleep enable bit (13). Where that register is comes
// from the Fixed ACPI Description Table (FADT, signature "FACP"); which
// sleep type means "off" (state S5) is not in any table but in the DSDT's
// AML bytecode, as a package named \_S5. Resetting the machine is a write of
// a given value to the FADT's reset register. (ACPI 6.5, sections 4.8.3,
// 5.2.9 and 7.4.2.)

#define FADT_DSDT          40   // 32-bit physical address of the DSDT
#define FADT_SMI_CMD       48   // I/O port that switches the firmware into ACPI mode...
#define FADT_ACPI_ENABLE   52   // ...when this value is written to it
#define FADT_PM1A_CNT      64   // I/O port of the PM1a control register
#define FADT_PM1B_CNT      68   // ...and of PM1b (0: there is none)
#define FADT_FLAGS        112
#define FADT_RESET_REG    116   // A Generic Address Structure: space (0 memory, 1 I/O), widths, then the address at +4
#define FADT_RESET_VALUE  128
#define FADT_X_DSDT       140   // 64-bit address of the DSDT, where the table is long enough to have it
#define FADT_RESET_SUPPORTED (1u << 10)

#define PM1_SCI_EN      (1u << 0)   // Set: the machine is in ACPI mode
#define PM1_SLP_TYP_SHIFT 10
#define PM1_SLP_EN      (1u << 13)

static struct {
    bool found;                 // A FADT with a PM1a control register
    uint16_t pm1a_cnt, pm1b_cnt;
    uint16_t smi_cmd;
    uint8_t acpi_enable;
    bool s5_known;              // \_S5 was found in the DSDT
    uint8_t s5_a, s5_b;         // Its sleep types for PM1a and PM1b
    bool reset_io;              // A reset register in I/O space
    uint16_t reset_port;
    uint8_t reset_value;
} power;

// Read one AML integer at `at` (ACPI 6.5, 20.2.3): Zero, One, or a byte or
// word with its prefix. Returns the number of bytes it took, 0 if `at` holds
// something else or runs past `end`.
static size_t aml_integer(const uint8_t *at, const uint8_t *end, uint32_t *value) {
    if (at >= end) return 0;
    if (at[0] == 0x00 || at[0] == 0x01) {          // ZeroOp, OneOp
        *value = at[0];
        return 1;
    }
    if (at[0] == 0x0A && at + 1 < end) {           // BytePrefix
        *value = at[1];
        return 2;
    }
    if (at[0] == 0x0B && at + 2 < end) {           // WordPrefix
        *value = (uint32_t)at[1] | ((uint32_t)at[2] << 8);
        return 3;
    }
    return 0;
}

bool acpi_parse_s5(const uint8_t *aml, size_t length, uint8_t *typ_a, uint8_t *typ_b) {
    if (!aml) return false;
    const uint8_t *end = aml + length;
    // "Name (\_S5, Package () { a, b, ... })" in bytecode: NameOp (0x08),
    // perhaps the root prefix '\', the name "_S5_", PackageOp (0x12), the
    // package length (one to four bytes: the top two bits of the first say
    // how many follow), the element count, then the elements
    for (const uint8_t *p = aml + 1; p + 4 <= end; p++) {
        if (p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_') continue;
        bool named = p[-1] == 0x08 || (p[-1] == '\\' && p > aml + 1 && p[-2] == 0x08);
        const uint8_t *q = p + 4;
        if (!named || q >= end || *q != 0x12) continue;
        q++;
        if (q >= end) return false;
        size_t header = 1 + (*q >> 6) + 1;         // The package length, then the number of elements
        if ((size_t)(end - q) <= header) return false;
        q += header;
        uint32_t a, b;
        size_t used = aml_integer(q, end, &a);
        if (used == 0) return false;
        size_t used_b = aml_integer(q + used, end, &b);
        if (used_b == 0) b = 0;                    // One element: PM1b has none to be given
        if (a > 7 || b > 7) return false;          // SLP_TYP is three bits
        *typ_a = (uint8_t)a;
        *typ_b = (uint8_t)b;
        return true;
    }
    return false;
}

// The little-endian 32-bit value at `offset` in `table` (FADT fields are
// not all naturally aligned)
static uint32_t get32_at(const uint8_t *table, size_t offset) {
    return (uint32_t)table[offset] | ((uint32_t)table[offset + 1] << 8) |
           ((uint32_t)table[offset + 2] << 16) | ((uint32_t)table[offset + 3] << 24);
}

// Record what powering off and resetting need from the FADT, and find the
// sleep type for S5 in the DSDT it points to
static void parse_fadt(const struct acpi_sdt_header *fadt) {
    const uint8_t *t = (const uint8_t *)fadt;
    if (fadt->length < FADT_PM1B_CNT + 4) return;
    power.pm1a_cnt = (uint16_t)get32_at(t, FADT_PM1A_CNT);
    power.pm1b_cnt = (uint16_t)get32_at(t, FADT_PM1B_CNT);
    power.smi_cmd = (uint16_t)get32_at(t, FADT_SMI_CMD);
    power.acpi_enable = t[FADT_ACPI_ENABLE];
    power.found = power.pm1a_cnt != 0;

    if (fadt->length >= FADT_RESET_VALUE + 1 && (get32_at(t, FADT_FLAGS) & FADT_RESET_SUPPORTED) &&
        t[FADT_RESET_REG] == 1) {                  // I/O space: the only kind used here
        power.reset_io = true;
        power.reset_port = (uint16_t)get32_at(t, FADT_RESET_REG + 4);
        power.reset_value = t[FADT_RESET_VALUE];
    }

    uint64_t dsdt_phys = get32_at(t, FADT_DSDT);
    if (fadt->length >= FADT_X_DSDT + 8) {
        uint64_t x = (uint64_t)get32_at(t, FADT_X_DSDT) | ((uint64_t)get32_at(t, FADT_X_DSDT + 4) << 32);
        if (x) dsdt_phys = x;
    }
    if (dsdt_phys) {
        const struct acpi_sdt_header *dsdt = phys_to_virt(dsdt_phys);
        if (signature_is(dsdt->signature, "DSDT") && dsdt->length > sizeof(*dsdt)) {
            power.s5_known = acpi_parse_s5((const uint8_t *)dsdt + sizeof(*dsdt), dsdt->length - sizeof(*dsdt),
                                           &power.s5_a, &power.s5_b);
        }
    }
    kprintf("ACPI: power control at port 0x%x, sleep type for off %s, reset %s\n", power.pm1a_cnt,
            power.s5_known ? "found" : "not found", power.reset_io ? "by register" : "by the keyboard controller");
}

bool acpi_can_power_off(void) {
    return power.found && power.s5_known;
}

void acpi_power_off(void) {
    if (!acpi_can_power_off()) return;

    // Some firmware starts in its own mode and has to be asked to hand the
    // power registers to the operating system first
    if (!(inw(power.pm1a_cnt) & PM1_SCI_EN) && power.smi_cmd && power.acpi_enable) {
        outb(power.smi_cmd, power.acpi_enable);
        for (int i = 0; i < 1000000 && !(inw(power.pm1a_cnt) & PM1_SCI_EN); i++) asm volatile("pause");
    }
    outw(power.pm1a_cnt, (uint16_t)((power.s5_a << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN));
    if (power.pm1b_cnt) outw(power.pm1b_cnt, (uint16_t)((power.s5_b << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN));
    for (int i = 0; i < 1000000; i++) asm volatile("pause"); // The power takes a moment to go
}

void acpi_reset(void) {
    if (power.reset_io) {
        outb(power.reset_port, power.reset_value);
        for (int i = 0; i < 1000000; i++) asm volatile("pause");
    }
    // Failing that, the keyboard controller's command 0xFE pulses the CPU's
    // reset line: how PCs have been reset since the AT
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) asm volatile("pause");
    outb(0x64, 0xFE);
    for (int i = 0; i < 1000000; i++) asm volatile("pause");
}

bool acpi_init(void *rsdp_ptr) {
    if (!rsdp_ptr) {
        kprintf("ACPI: No RSDP from bootloader; assuming identity ISA IRQ routing\n");
        return false;
    }

    const struct acpi_rsdp *rsdp = rsdp_ptr;
    if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' || rsdp->signature[2] != 'D') {
        kprintf("ACPI: the RSDP has no signature; assuming identity ISA IRQ routing\n");
        return false;
    }

    // Prefer the XSDT (64-bit pointers) when the firmware provides one
    bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const struct acpi_sdt_header *root =
        phys_to_virt(use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address);
    size_t entry_size = use_xsdt ? 8 : 4;
    if (root->length < sizeof(struct acpi_sdt_header)) {
        kprintf("ACPI: the root table is shorter than its header; ignored\n");
        return false;
    }
    size_t entry_count = (root->length - sizeof(struct acpi_sdt_header)) / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(struct acpi_sdt_header);
    bool found_madt = false;

    for (size_t i = 0; i < entry_count; i++) {
        uint64_t table_phys = use_xsdt
            ? *(const uint64_t *)(entries + i * 8)
            : *(const uint32_t *)(entries + i * 4);
        if (table_phys == 0) continue;
        const struct acpi_sdt_header *table = phys_to_virt(table_phys);

        if (signature_is(table->signature, "APIC") && table->length >= sizeof(struct acpi_madt)) {
            parse_madt((const struct acpi_madt *)table);
            found_madt = true;
        } else if (signature_is(table->signature, "FACP")) {
            parse_fadt(table);
        }
    }

    if (!found_madt) kprintf("ACPI: MADT not found; assuming identity ISA IRQ routing\n");
    return found_madt;
}

bool acpi_ioapic(uint64_t *phys, uint32_t *gsi_base) {
    if (!ioapic_found) return false;
    *phys = ioapic_address;
    *gsi_base = ioapic_gsi_base;
    return true;
}

uint32_t acpi_isa_irq_to_gsi(uint8_t irq, uint16_t *out_flags) {
    if (irq < 16 && isa_overrides[irq].present) {
        if (out_flags) *out_flags = isa_overrides[irq].flags;
        return isa_overrides[irq].gsi;
    }
    if (out_flags) *out_flags = 0;
    return irq;
}
