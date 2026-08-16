/**
 * @file socket.c
 * @brief Implementation of Network Socket Layer and Checksumming
 */

#include "socket.h"
#include "heap.h"
#include "virtio_net.h"
#include "string_util.h"

uint16_t net_checksum(const void *data, size_t len) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len > 0) {
        sum += *(const uint8_t *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

socket_t *socket_create(uint16_t port) {
    socket_t *sock = (socket_t *)kmalloc(sizeof(socket_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(socket_t));

    sock->src_ip = 0x0A00020F; // Default QEMU guest IP: 10.0.2.15
    sock->src_port = port;

    return sock;
}

int socket_sendto(socket_t *sock, uint32_t dst_ip, uint16_t dst_port, const void *buf, size_t len) {
    if (!sock || !buf || len == 0) return 0;
    sock->dst_ip = dst_ip;
    sock->dst_port = dst_port;

    // Transmit raw ethernet packet frame via VirtIO Network Card
    virtio_net_send_packet(buf, len);
    return (int)len;
}
