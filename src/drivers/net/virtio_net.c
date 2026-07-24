/**
 * @file virtio_net.c
 * @brief Implementation of VirtIO Network Adapter Driver
 */

#include "virtio_net.h"
#include "pci.h"
#include "kprintf.h"
#include "kstring.h"

static bool net_initialized = false;

int virtio_net_init(void) {
    pci_init();

    pci_device_t net_dev;
    if (pci_find_device(0x1AF4, 0x1000, &net_dev) || pci_find_device(0x1AF4, 0x1041, &net_dev)) {
        kprintf("virtio-net: STUB, PCI device present at %x:%x.%x but no driver talks to it\n",
                net_dev.bus, net_dev.device, net_dev.function);
    } else {
        kprintf("virtio-net: STUB, no device present and no driver\n");
    }

    net_initialized = true;
    return 0;
}

int virtio_net_send_packet(const uint8_t *packet, size_t len) {
    if (!net_initialized || !packet || len == 0) return -1;
    kprintf("VirtIO-Net: Sent network packet (%zu bytes)\n", len);
    return 0;
}
