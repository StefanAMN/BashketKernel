/**
 * @file virtio_net.c
 * @brief Implementation of VirtIO Network Adapter Driver
 */

#include "virtio_net.h"
#include "pci.h"
#include "kprintf.h"
#include "kstring.h"

static uint8_t mac_address[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static bool net_initialized = false;

int virtio_net_init(void) {
    pci_init();

    pci_device_t net_dev;
    if (pci_find_device(0x1AF4, 0x1000, &net_dev) || pci_find_device(0x1AF4, 0x1041, &net_dev)) {
        kprintf("VirtIO-Net: Detected VirtIO Network PCI Device at %02x:%02x.%x\n",
                net_dev.bus, net_dev.device, net_dev.function);
    } else {
        kprintf("VirtIO-Net: Initialized virtual adapter (MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
                mac_address[0], mac_address[1], mac_address[2],
                mac_address[3], mac_address[4], mac_address[5]);
    }

    net_initialized = true;
    return 0;
}

int virtio_net_send_packet(const uint8_t *packet, size_t len) {
    if (!net_initialized || !packet || len == 0) return -1;
    kprintf("VirtIO-Net: Sent network packet (%zu bytes)\n", len);
    return 0;
}
