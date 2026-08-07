/**
 * @file virtio_pci.h
 * @brief A virtio device on the PCI bus: finding its registers and the
 *        handshake every driver starts with
 *
 * virtio devices come with two register interfaces, and a device may offer
 * either or both (virtio 1.2 specification, section 4.1):
 *
 *   legacy   A block of I/O ports named by BAR 0, with a fixed layout. A
 *            queue's three rings must be in one piece of memory whose page
 *            number fits in 32 bits. This is what virtio was before it had
 *            a standard.
 *
 *   modern   Registers in memory rather than I/O ports, in up to four
 *            separate structures: common configuration, notification,
 *            interrupt status and device-specific configuration. Where
 *            each one is (which BAR, what offset, how long) is told by
 *            entries in the device's PCI capability list. Feature bits are
 *            64 wide, a queue's rings are given by three 64-bit addresses,
 *            and the driver must accept the feature VIRTIO_F_VERSION_1.
 *
 * This file hides the difference: a driver opens the device, negotiates
 * features, sets up its queues (virtqueue.h) and reads its configuration
 * through the functions here, whichever interface the device has. The
 * modern one is used when it is offered.
 *
 * The one thing a driver must still know: with the modern interface the
 * header in front of every network frame is two bytes longer.
 */

#ifndef VIRTIO_PCI_H
#define VIRTIO_PCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pci.h"

#define VIRTIO_VENDOR 0x1AF4

// Device types (virtio 1.2, section 5)
#define VIRTIO_TYPE_NET   1
#define VIRTIO_TYPE_BLOCK 2

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01 // "I have noticed you"
#define VIRTIO_STATUS_DRIVER      0x02 // "I know how to drive you"
#define VIRTIO_STATUS_DRIVER_OK   0x04 // "I am ready"
#define VIRTIO_STATUS_FEATURES_OK 0x08 // "These are the features I will use" (modern only)
#define VIRTIO_STATUS_FAILED      0x80

#define VIRTIO_F_VERSION_1 (1ULL << 32) // The device follows the standard, not the legacy behaviour

// One of the modern interface's structures: where it is
typedef struct {
    uint8_t bar;        // Which of the device's six base address registers
    uint32_t offset;    // Where in that region
    uint32_t length;
    bool found;
} virtio_cap_t;

typedef struct {
    virtio_cap_t common, notify, isr, device;
    uint32_t notify_multiplier;  // A queue's notify address is its notify offset times this
} virtio_caps_t;

typedef struct virtio_device {
    pci_device_t pci;
    bool modern;
    uint8_t irq;                 // The interrupt line named in its configuration space

    uint16_t io_base;            // Legacy: the I/O port block

    // Modern: the four structures, mapped
    volatile uint8_t *common;
    volatile uint8_t *notify;
    volatile uint8_t *isr;
    volatile uint8_t *device_config;
    uint32_t notify_length, device_config_length;
    uint32_t notify_multiplier;
} virtio_device_t;

/**
 * @brief Find the n-th (from 0) virtio device of a type on the bus, whether
 *        it has the device id of one that still offers the legacy interface
 *        (0x1000 + type - 1) or of a modern-only one (0x1040 + type).
 */
bool virtio_pci_find(uint16_t type, unsigned n, pci_device_t *out);

/**
 * @brief Read the modern interface's structures out of a device's PCI
 *        capability list. The list comes from the device, so nothing in it
 *        is believed: it may loop, name a BAR that does not exist, or give
 *        lengths too short to hold the registers.
 * @param read8, read32 Read the device's configuration space (a parameter
 *        so that the tests can supply one of their own).
 * @return true if all four structures were found and are usable.
 */
bool virtio_pci_parse_caps(uint8_t (*read8)(void *ctx, uint8_t offset), uint32_t (*read32)(void *ctx, uint8_t offset),
                           void *ctx, virtio_caps_t *out);

/**
 * @brief Take charge of a device: choose its interface, map its registers,
 *        reset it and announce a driver (ACKNOWLEDGE, DRIVER).
 * @return false if it offers no interface this kernel can use.
 */
bool virtio_open(virtio_device_t *dev, const pci_device_t *pci);

/**
 * @brief Agree the features to use: those of `wanted` the device offers.
 *        (VERSION_1 is added for the modern interface, which requires it.)
 * @param accepted Receives what was agreed. May be NULL.
 * @return false if the device refuses, which leaves it marked FAILED.
 */
bool virtio_negotiate(virtio_device_t *dev, uint64_t wanted, uint64_t *accepted);

/**
 * @brief Tell the device the driver is ready (after its queues are set up),
 *        or that it has given up.
 */
void virtio_driver_ok(virtio_device_t *dev);
void virtio_fail(virtio_device_t *dev);

/**
 * @brief Read the interrupt status, which also makes the device lower its
 *        interrupt line. Bit 0: a queue has something for the driver.
 */
uint8_t virtio_isr(virtio_device_t *dev);

/**
 * @brief Read the device-specific configuration (a disk's size, a network
 *        card's address). Out of range reads give 0.
 */
uint8_t virtio_config8(virtio_device_t *dev, uint32_t offset);
uint64_t virtio_config64(virtio_device_t *dev, uint32_t offset);

/**
 * @brief "modern" or "legacy", for boot messages.
 */
const char *virtio_interface_name(const virtio_device_t *dev);

#endif // VIRTIO_PCI_H
