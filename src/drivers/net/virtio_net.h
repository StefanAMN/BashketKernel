/**
 * @file virtio_net.h
 * @brief virtio network card driver
 *
 * Drives the network card QEMU provides with `-device virtio-net-pci`,
 * through the legacy virtio PCI interface. The driver moves whole Ethernet
 * frames; what is inside them is the business of the network stack (net.h).
 *
 * Receiving: a set of buffers is kept posted on the receive queue. The card
 * fills one per frame and raises an interrupt; virtio_net_poll hands the
 * frame over and posts the buffer again.
 *
 * Sending: the frame is copied into a free transmit buffer and queued.
 * Buffers are reclaimed the next time something is sent.
 */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ETH_FRAME_MAX 1514 // 14-byte header + 1500 bytes of payload
#define ETH_FRAME_MIN 60   // Shorter frames are padded with zeros

/**
 * @brief Find and set up the first virtio network card: its queues, its
 *        address (made up if it gives none), receive buffers, interrupt.
 *        Called once at boot.
 * @return 0 if a card is ready, -1 if there is none or it could not be set up.
 */
int virtio_net_init(void);

/**
 * @brief Whether a card was set up.
 */
bool virtio_net_present(void);

/**
 * @brief The card's hardware (MAC) address.
 */
void virtio_net_get_mac(uint8_t mac[6]);

/**
 * @brief Queue one Ethernet frame for sending.
 * @return 0, or -1 if there is no card, the length is not a valid frame
 *         length, or every transmit buffer is in use (the frame is dropped,
 *         as a busy network would drop it).
 */
int virtio_net_send(const void *frame, size_t len);

/**
 * @brief Take one received frame, if there is one.
 * @return Its length, or 0 if nothing has arrived. Never sleeps.
 */
size_t virtio_net_poll(void *frame, size_t max);

/**
 * @brief Sleep (uninterruptibly: for the receive thread) until the card has
 *        a received frame waiting.
 */
void virtio_net_wait_rx(void);

/**
 * @brief Frames received since boot.
 */
uint64_t virtio_net_rx_count(void);

/**
 * @brief Frames sent since boot.
 */
uint64_t virtio_net_tx_count(void);

#endif // VIRTIO_NET_H
