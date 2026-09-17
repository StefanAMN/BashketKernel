/**
 * @file pci.h
 * @brief The PCI bus: finding devices and reading their configuration space
 *
 * Only what the virtio drivers need: bus 0 to 255 are scanned once, function
 * 0 of each device slot only (multi-function devices show their first
 * function), through the legacy configuration ports 0xCF8/0xCFC. Those are
 * one index/data pair, used unlocked: every caller runs at boot.
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
} pci_device_t;

// Configuration space registers (offsets)
#define PCI_COMMAND        0x04
#define PCI_BAR0           0x10
#define PCI_INTERRUPT_LINE 0x3C

/**
 * @brief Scan the bus and remember up to 32 devices. Later calls do nothing.
 */
void pci_init(void);

/**
 * @brief Read the 32-bit configuration register containing byte `offset`
 *        of the device (the low two bits of `offset` are ignored).
 */
uint32_t pci_read32(const pci_device_t *dev, uint8_t offset);

/**
 * @brief Read the 16 bits at `offset` (which must be even).
 */
uint16_t pci_read16(const pci_device_t *dev, uint8_t offset);

/**
 * @brief Read the byte at `offset`.
 */
uint8_t pci_read8(const pci_device_t *dev, uint8_t offset);

/**
 * @brief Write the 16 bits at (even) `offset`, keeping the rest of the
 *        32-bit register it is part of.
 */
void pci_write16(const pci_device_t *dev, uint8_t offset, uint16_t value);

/**
 * @brief Let a device use its I/O ports and do DMA (command register bits
 *        0 and 2).
 */
void pci_enable_io_and_bus_master(const pci_device_t *dev);

/**
 * @brief Let a device use its memory-mapped registers and do DMA (command
 *        register bits 1 and 2).
 */
void pci_enable_memory_and_bus_master(const pci_device_t *dev);

/**
 * @brief The n-th device (from 0) the scan found with these vendor and
 *        device ids, copied into *out_dev (if not NULL).
 * @return false if there are not that many.
 */
bool pci_find_nth_device(uint16_t vendor_id, uint16_t device_id, unsigned n, pci_device_t *out_dev);

#endif // PCI_H
