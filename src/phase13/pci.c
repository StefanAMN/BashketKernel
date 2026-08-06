/**
 * @file pci.c
 * @brief Implementation of PCI Bus Enumerator
 */

#include "pci.h"
#include "kprintf.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define MAX_PCI_DEVICES 32

static pci_device_t pci_devices[MAX_PCI_DEVICES];
static size_t pci_device_count = 0;

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("out dx, eax" : : "a"(val), "d"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("in eax, dx" : "=a"(ret) : "d"(port));
    return ret;
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_init(void) {
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
