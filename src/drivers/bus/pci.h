/**
 * @file pci.h
 * @brief PCI Bus Enumerator and Configuration Space Interface
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
} pci_device_t;

/**
 * @brief Scan PCI bus for connected devices.
 */
// Configuration space registers (offsets)
#define PCI_COMMAND        0x04
#define PCI_BAR0           0x10
#define PCI_INTERRUPT_LINE 0x3C

void pci_init(void);

/**
 * @brief Read or write a device's configuration space.
 */
uint32_t pci_read32(const pci_device_t *dev, uint8_t offset);
uint16_t pci_read16(const pci_device_t *dev, uint8_t offset);
uint8_t pci_read8(const pci_device_t *dev, uint8_t offset);
void pci_write32(const pci_device_t *dev, uint8_t offset, uint32_t value);
void pci_write16(const pci_device_t *dev, uint8_t offset, uint16_t value);

/**
 * @brief Let a device use its I/O ports and do DMA.
 */
void pci_enable_io_and_bus_master(const pci_device_t *dev);

/**
 * @brief Find PCI device matching vendor and device IDs.
 * @param vendor_id Target PCI vendor ID.
 * @param device_id Target PCI device ID.
 * @param out_dev Output struct pointer to copy device info.
 * @return true if device found, false otherwise.
 */
bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev);

#endif // PCI_H
