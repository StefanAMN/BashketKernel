/**
 * @file socket.h
 * @brief Network Socket Abstraction & UDP/IP Protocol Stack Interface
 */

#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
} socket_t;

/**
 * @brief Create a new UDP network socket bound to a local port.
 * @param port Local UDP port number.
 * @return Pointer to allocated socket_t structure.
 */
socket_t *socket_create(uint16_t port);

/**
 * @brief Send a UDP packet payload over network adapter.
 * @param sock Active socket structure.
 * @param dst_ip Destination IPv4 address in network order.
 * @param dst_port Destination UDP port.
 * @param buf Data payload buffer.
 * @param len Byte length of data.
 * @return Number of bytes transmitted.
 */
int socket_sendto(socket_t *sock, uint32_t dst_ip, uint16_t dst_port, const void *buf, size_t len);

/**
 * @brief Calculate 16-bit Internet Checksum over header/payload buffer.
 * @param data Data buffer pointer.
 * @param len Byte length.
 * @return 16-bit 1's complement checksum value.
 */
uint16_t net_checksum(const void *data, size_t len);

#endif // SOCKET_H
