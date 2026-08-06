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
void pci_init(void);

/**
 * @brief Find PCI device matching vendor and device IDs.
 * @param vendor_id Target PCI vendor ID.
 * @param device_id Target PCI device ID.
 * @param out_dev Output struct pointer to copy device info.
 * @return true if device found, false otherwise.
 */
bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev);

#endif // PCI_H
