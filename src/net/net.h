/**
 * @file net.h
 * @brief A small IPv4 network stack: Ethernet, ARP, IPv4, ICMP echo, UDP
 *
 * How a datagram travels down and up:
 *
 *   UDP       ports: which program on the machine
 *   IPv4      addresses: which machine, possibly several networks away
 *   Ethernet  hardware (MAC) addresses: which card on this cable
 *   ARP       asks the local network "who has this IPv4 address?" to learn
 *             the MAC address to put on the Ethernet frame
 *   ICMP      control messages carried in IPv4; here only echo ("ping")
 *
 * The configuration is fixed: address 10.0.2.15, netmask 255.255.255.0,
 * gateway 10.0.2.2, which is what QEMU's user-mode network provides. There
 * is no IPv6 and no routing table beyond "local network or gateway".
 * Datagrams larger than a frame are sent in fragments and reassembled on
 * arrival (ipfrag.h).
 *
 * Received frames are handled by a kernel thread (netd). Everything in a
 * received frame is untrusted and checked against the frame's real length.
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// An IPv4 address as a number, most significant byte first: NET_IP(10,0,2,15)
#define NET_IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

// The configuration the stack starts with: what QEMU's user-mode network
// expects of its guest. It can be replaced at run time (net_set_config), by
// hand or with what a DHCP server says (/bin/dhcp).
#define NET_LOCAL_IP NET_IP(10, 0, 2, 15)
#define NET_NETMASK  NET_IP(255, 255, 255, 0)
#define NET_GATEWAY  NET_IP(10, 0, 2, 2)
#define NET_DNS      NET_IP(10, 0, 2, 3)
#define NET_BROADCAST 0xFFFFFFFFu

// The interface's settings. An address of 0 means "not configured": then
// only broadcasts can be sent (from 0.0.0.0), and anything addressed to our
// card is taken in, which is exactly what asking a DHCP server needs.
typedef struct {
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;                // Where the resolver in user space sends its questions
    uint8_t mac[6];              // Read only: the card's address
    uint8_t padding[2];
} net_config_t;

#define UDP_PAYLOAD_MAX 65507  // 65535 (IPv4 total length) - 20 (IPv4 header) - 8 (UDP header): sent in fragments above 1472
#define UDP_QUEUE_BYTES 65536  // What a socket holds before new datagrams are dropped: each
                               // costs its payload plus UDP_DATAGRAM_OVERHEAD, so that
                               // empty ones are not free
#define UDP_DATAGRAM_OVERHEAD 16
#define UDP_MAX_SOCKETS 16

typedef struct {
    uint64_t frames_in;          // Frames given to the stack
    uint64_t dropped;            // Malformed, not for us, or not understood
    uint64_t arp_replies_sent;
    uint64_t arp_requests_sent;  // Questions asked, first ones and refreshes of old answers
    uint64_t echo_replies_sent;
    uint64_t udp_delivered;      // Datagrams put on a socket's queue
    uint64_t udp_no_socket;      // Datagrams for a port nobody listens on
    uint64_t udp_queue_full;
    uint64_t fragmented_sent;    // Datagrams sent in fragments (ipfrag.h has the receiving side's counters)
    uint64_t icmp_errors_sent;   // Unreachable and time exceeded messages we sent
    uint64_t icmp_errors_limited; // ... and did not send, over the rate limit
    uint64_t icmp_errors_received;
} net_stats_t;

typedef struct udp_socket udp_socket_t;

/**
 * @brief Start the stack (and its receive thread) if a network card exists.
 */
void net_init(void);

/**
 * @brief Whether the stack is running (there is a network card).
 */
bool net_up(void);

/**
 * @brief The stack's counters since boot (read without a lock: for tests
 *        and diagnostics).
 */
const net_stats_t *net_stats(void);

/**
 * @brief The current settings (always succeeds; zeros if there is no card).
 */
void net_get_config(net_config_t *out);

/**
 * @brief Replace address, netmask, gateway and DNS server (the MAC is
 *        ignored). What was learned about neighbours is forgotten.
 * @return 0, or -EINVAL for settings that can not work (a gateway outside
 *         the network, a netmask with holes).
 */
int net_set_config(const net_config_t *config);

/**
 * @brief The Internet checksum (RFC 1071) of a block of bytes.
 */
uint16_t net_checksum(const void *data, size_t len);

// --- For the transport protocols (UDP here, TCP in tcp.c) --------------------

#define IP_PROTO_UDP 17
#define IP_PROTO_TCP 6

/**
 * @brief The address our packets to `dst` carry as their source: `dst`
 *        itself for a loopback address, else ours.
 */
uint32_t net_source_for(uint32_t dst);

/**
 * @brief Checksum of a UDP or TCP segment (`len` bytes), including the
 *        pseudo header of addresses, protocol and length. 0 when checking
 *        a segment that carries its correct checksum.
 */
uint16_t net_transport_checksum(uint8_t protocol, uint32_t src_ip, uint32_t dst_ip, const void *segment, size_t len);

/**
 * @brief Send `len` bytes as the payload of an IPv4 packet to `dst`: to
 *        ourselves straight back through the input path, else to the
 *        machine or the gateway, whichever is the next hop. Never call with
 *        a lock held.
 * @param can_block The caller may sleep waiting for an ARP answer (the
 *        receive thread may not: it would be waiting for itself).
 * @return 0, -EMSGSIZE, -ENETDOWN, -EHOSTUNREACH (or no ARP answer known,
 *         when it may not wait), -EINTR.
 */
int net_ipv4_send(uint32_t dst, uint8_t protocol, const void *payload, size_t len, bool can_block);

/**
 * @brief Handle one received Ethernet frame. Called by the receive thread;
 *        exposed so that tests can present frames of their own.
 */
void net_handle_frame(const uint8_t *frame, size_t len);

/**
 * @brief Send an ICMP echo request and wait for the reply.
 * @param rtt_ticks If non-NULL, receives the round-trip time in timer ticks.
 * @return 0, -ETIMEDOUT, -EHOSTUNREACH (no answer to ARP), -ENETDOWN,
 *         or -EINTR.
 */
int net_ping(uint32_t ip, uint64_t timeout_ticks, uint64_t *rtt_ticks);

/**
 * @brief True if the hardware address for `ip` is known (ARP cache).
 */
bool net_arp_known(uint32_t ip);

#ifdef KERNEL_TESTS
/**
 * @brief For tests: make the ARP cache's answer for `ip` `ticks` older, as
 *        if time had passed. False if there is no answer for `ip`.
 */
bool net_arp_age(uint32_t ip, uint64_t ticks);
#endif

// --- UDP sockets -------------------------------------------------------------

/**
 * @brief Create a socket (NULL if there are too many or memory ran out).
 */
udp_socket_t *udp_open(void);

/**
 * @brief Choose the port the socket receives on.
 * @return 0, -EADDRINUSE, or -EINVAL (port 0, or already bound).
 */
int udp_bind(udp_socket_t *sock, uint16_t port);

/**
 * @brief Close the socket: datagrams queued for it are thrown away, and a
 *        task waiting in udp_recvfrom gets -EBADF.
 */
void udp_close(udp_socket_t *sock);

/**
 * @brief Send one datagram. An unbound socket is given a free port first.
 * @return Bytes sent, -EMSGSIZE (more than UDP_PAYLOAD_MAX), -EHOSTUNREACH,
 *         -ENETDOWN, or -EINTR.
 */
int64_t udp_sendto(udp_socket_t *sock, uint32_t ip, uint16_t port, const void *data, size_t len);

/**
 * @brief Receive one datagram into the kernel buffer `buf`, sleeping until
 *        one arrives.
 * @param timeout_ticks 0 waits for ever.
 * @return Bytes copied (a longer datagram is cut to `len`), -ETIMEDOUT,
 *         -EINVAL (socket not bound), or -EINTR.
 */
int64_t udp_recvfrom(udp_socket_t *sock, void *buf, size_t len, uint32_t *src_ip, uint16_t *src_port, uint64_t timeout_ticks);

/**
 * @brief For poll(): POLLIN when a datagram is queued; always POLLOUT.
 */
int udp_poll(udp_socket_t *sock);

#endif // NET_H
