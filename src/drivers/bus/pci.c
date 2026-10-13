/**
 * @file pci.c
 * @brief Implementation of PCI Bus Enumerator
 */

#include "pci.h"
#include "kprintf.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define MAX_PCI_DEVICES 32

static pci_device_t pci_devices[MAX_PCI_DEVICES];
static size_t pci_device_count = 0;

// Configuration space is reached through two I/O ports: write the address of
// a 32-bit register (bus, device, function, offset) to 0xCF8, then read or
// write its contents at 0xCFC.
static uint32_t config_address(const pci_device_t *dev, uint8_t offset) {
    return 0x80000000u | ((uint32_t)dev->bus << 16) | ((uint32_t)dev->device << 11) |
           ((uint32_t)dev->function << 8) | (offset & 0xFC);
}

uint32_t pci_read32(const pci_device_t *dev, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, config_address(dev, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(const pci_device_t *dev, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, config_address(dev, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_read16(const pci_device_t *dev, uint8_t offset) {
    return (uint16_t)(pci_read32(dev, offset) >> ((offset & 2) * 8));
}

uint8_t pci_read8(const pci_device_t *dev, uint8_t offset) {
    return (uint8_t)(pci_read32(dev, offset) >> ((offset & 3) * 8));
}

void pci_write16(const pci_device_t *dev, uint8_t offset, uint16_t value) {
    // Registers are 32 bits wide: change the wanted half, keep the other
    uint32_t shift = (offset & 2) * 8;
    uint32_t current = pci_read32(dev, offset);
    current = (current & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_write32(dev, offset, current);
}

void pci_enable_io_and_bus_master(const pci_device_t *dev) {
    // Command register: bit 0 lets the device respond on its I/O ports,
    // bit 2 lets it read and write memory by itself (DMA)
    pci_write16(dev, PCI_COMMAND, pci_read16(dev, PCI_COMMAND) | 0x0005);
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    pci_device_t where = { .bus = bus, .device = dev, .function = func };
    return pci_read32(&where, offset);
}

void pci_init(void) {
    static bool scanned = false;
    if (scanned) return; // Devices do not come and go; one scan is enough
    scanned = true;

    pci_device_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id_reg = pci_config_read32((uint8_t)bus, dev, 0, 0);
            uint16_t vendor_id = (uint16_t)(id_reg & 0xFFFF);
            uint16_t device_id = (uint16_t)(id_reg >> 16);

            if (vendor_id != 0xFFFF && pci_device_count < MAX_PCI_DEVICES) {
                pci_devices[pci_device_count].bus = (uint8_t)bus;
                pci_devices[pci_device_count].device = dev;
                pci_devices[pci_device_count].function = 0;
                pci_devices[pci_device_count].vendor_id = vendor_id;
                pci_devices[pci_device_count].device_id = device_id;
                pci_device_count++;
            }
        }
    }

    kprintf("PCI Init: Bus scan complete (%zu devices detected)\n", pci_device_count);
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev) {
    for (size_t i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id && pci_devices[i].device_id == device_id) {
            if (out_dev) *out_dev = pci_devices[i];
            return true;
        }
    }
    return false;
}

bool pci_find_nth_device(uint16_t vendor_id, uint16_t device_id, unsigned n, pci_device_t *out_dev) {
    for (size_t i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id != vendor_id || pci_devices[i].device_id != device_id) continue;
        if (n-- == 0) {
            if (out_dev) *out_dev = pci_devices[i];
            return true;
        }
    }
    return false;
}
