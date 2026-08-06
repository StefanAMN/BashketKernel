/**
 * @file virtio_net.h
 * @brief VirtIO Network Card Driver Interface
 */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize VirtIO Network adapter.
 * @return 0 on success, negative error code on failure.
 */
int virtio_net_init(void);

/**
 * @brief Send Ethernet packet over VirtIO network interface.
 * @param packet Packet bytes.
 * @param len Packet length.
 * @return 0 on success, negative error code on failure.
 */
int virtio_net_send_packet(const uint8_t *packet, size_t len);

#endif // VIRTIO_NET_H
