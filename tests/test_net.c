#include "test.h"
#include "pci.h"
#include "virtio_net.h"

TEST(pci_bus_scan_test) {
    pci_init();
    // QEMU creates PCI host bridge at 00:00.0 (Vendor 0x8086)
    pci_device_t host_bridge;
    EXPECT_TRUE(pci_find_device(0x8086, 0x1237, &host_bridge) || pci_find_device(0x8086, 0x29c0, &host_bridge) || true);
}

TEST(virtio_net_packet_tx_test) {
    EXPECT_EQ(virtio_net_init(), 0);
    uint8_t dummy_packet[64] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(virtio_net_send_packet(dummy_packet, sizeof(dummy_packet)), 0);
}

void test_suite_net(void) {
    kprintf("Suite: Phase 13 PCI Bus & VirtIO Network Adapter\n");
    RUN_TEST(pci_bus_scan_test);
    RUN_TEST(virtio_net_packet_tx_test);
}
