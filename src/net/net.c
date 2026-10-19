/**
 * @file net.c
 * @brief IPv4 network stack (see net.h)
 */

#include "net.h"
#include "tcp.h"
#include "virtio_net.h"
#include "sched.h"
#include "pit.h"
#include "heap.h"
#include "kprintf.h"
#include "kstring.h"
#include "spinlock.h"
#include "kerrno.h"
#include "random.h"

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define IP_PROTO_ICMP 1

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

#define ARP_REQUEST 1
#define ARP_REPLY   2

#define ARP_CACHE_SIZE 16
#define ARP_TRIES 3
#define ARP_WAIT_TICKS 30          // Per try: 0.3 s

#define EPHEMERAL_PORT_FIRST 49152 // Ports given to sockets that did not ask for one

// --- Wire formats (all multi-byte fields are big-endian) ------------------

struct eth_header {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
} __attribute__((packed));

struct arp_packet {
    uint16_t hardware_type;        // 1: Ethernet
    uint16_t protocol_type;        // 0x0800: IPv4
    uint8_t hardware_len;          // 6
    uint8_t protocol_len;          // 4
    uint16_t operation;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

struct ipv4_header {
    uint8_t version_ihl;           // Version (4) and header length in 32-bit words
    uint8_t tos;
    uint16_t total_length;         // Header and payload
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct icmp_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed));

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;               // Header and payload
    uint16_t checksum;             // 0: not computed
} __attribute__((packed));

// Reverse the byte order of 16 and 32-bit values (both are needed)
static uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
// (See swap16)
static uint32_t swap32(uint32_t v) {
    return (v << 24) | ((v << 8) & 0x00FF0000) | ((v >> 8) & 0x0000FF00) | (v >> 24);
}
// x86 is little-endian, the network is big-endian
#define htons(v) swap16(v)
#define ntohs(v) swap16(v)
#define htonl(v) swap32(v)
#define ntohl(v) swap32(v)

// --- State ---------------------------------------------------------------------

static bool up = false;
static uint8_t our_mac[6];

// Read far more often than written, and each field on its own: a reader
// that sees one field from before a change and one from after sends a
// packet that goes nowhere, which the network does anyway
static uint32_t local_ip = NET_LOCAL_IP;
static uint32_t local_netmask = NET_NETMASK;
static uint32_t local_gateway = NET_GATEWAY;
static uint32_t local_dns = NET_DNS;
static net_stats_t stats;
static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct {
    uint32_t ip;                   // 0: unused entry
    uint8_t mac[6];
    uint64_t last_used;
} arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

// A ping in progress: the reply handler matches on id and sequence
typedef struct {
    bool active;
    bool answered;
    uint16_t id, sequence;
} ping_wait_t;
static ping_wait_t pings[4];

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t len;
    uint8_t data[UDP_PAYLOAD_MAX];
} udp_datagram_t;

struct udp_socket {
    bool open;
    uint16_t port;                 // 0: not bound
    udp_datagram_t *queue;         // UDP_QUEUE_LEN entries
    size_t head, count;
};
static struct udp_socket sockets[UDP_MAX_SOCKETS];
static uint16_t next_ephemeral = EPHEMERAL_PORT_FIRST;

bool net_up(void) { return up; }

void net_get_config(net_config_t *out) {
    memset(out, 0, sizeof(*out));
    if (!up) return;
    out->ip = local_ip;
    out->netmask = local_netmask;
    out->gateway = local_gateway;
    out->dns = local_dns;
    memcpy(out->mac, our_mac, 6);
}
const net_stats_t *net_stats(void) { return &stats; }

// The Internet checksum (RFC 1071): add the data up as 16-bit big-endian
// numbers, fold the carries back in, and invert. A block that includes its
// own correct checksum sums to 0xFFFF, so checking is: checksum of the whole
// block == 0.
//
// checksum_add accumulates; several blocks can be summed one after another
// as long as all but the last have an even length.
static uint32_t checksum_add(uint32_t sum, const void *data, size_t len) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i + 1 < len; i += 2) sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1];
    if (len & 1) sum += (uint32_t)bytes[len - 1] << 8;
    return sum;
}

// Fold the carries of a checksum_add sum back in and invert it
static uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t net_checksum(const void *data, size_t len) {
    return checksum_finish(checksum_add(0, data, len));
}

uint32_t net_source_for(uint32_t dst) {
    // A packet to a loopback address comes from that address: the answer
    // then goes back to the name the other end was reached by
    return (dst >> 24) == 127 ? dst : local_ip;
}

// The UDP and TCP checksums also cover a "pseudo header" of fields from the
// IP header: both addresses, the protocol number and the segment length
uint16_t net_transport_checksum(uint8_t protocol, uint32_t src_ip, uint32_t dst_ip, const void *segment, size_t len) {
    uint8_t pseudo[12];
    uint32_t src = htonl(src_ip), dst = htonl(dst_ip);
    uint16_t length = htons((uint16_t)len);
    memcpy(pseudo + 0, &src, 4);
    memcpy(pseudo + 4, &dst, 4);
    pseudo[8] = 0;
    pseudo[9] = protocol;
    memcpy(pseudo + 10, &length, 2);
    return checksum_finish(checksum_add(checksum_add(0, pseudo, sizeof(pseudo)), segment, len));
}

// The UDP checksum of a datagram (header and payload) between two addresses
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *udp, size_t len) {
    return net_transport_checksum(IP_PROTO_UDP, src_ip, dst_ip, udp, len);
}

// --- ARP -----------------------------------------------------------------------

// One lock for the stack's tables: the ARP cache, the pings awaiting an
// answer, and the sockets with their queues. Not held while a frame is
// built or handed to the card.
static spinlock_t net_lock = SPINLOCK_RANKED("net", LOCK_RANK_NET);

// Remember that `ip` is at hardware address `mac` (replacing the entry
// unused for longest if the cache is full), and wake whoever waits for
// an answer. Takes net_lock.
static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    if (ip == 0 || ip == local_ip) return;

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    arp_entry_t *slot = NULL, *oldest = &arp_cache[0];
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip) { slot = &arp_cache[i]; break; }
        if (arp_cache[i].ip == 0 && !slot) slot = &arp_cache[i];
        if (arp_cache[i].last_used < oldest->last_used) oldest = &arp_cache[i];
    }
    if (!slot) slot = oldest; // Cache full: replace the entry unused for longest
    slot->ip = ip;
    memcpy(slot->mac, mac, 6);
    slot->last_used = timer_ticks();
    spinlock_release_irqrestore(&net_lock, flags);

    sched_wakeup(arp_cache); // Someone may be waiting for exactly this answer
}

// Caller holds net_lock
static bool arp_lookup_locked(uint32_t ip, uint8_t mac[6]) {
    bool found = false;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            arp_cache[i].last_used = timer_ticks();
            found = true;
            break;
        }
    }
    return found;
}

// arp_lookup_locked, taking net_lock
static bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    bool found = arp_lookup_locked(ip, mac);
    spinlock_release_irqrestore(&net_lock, flags);
    return found;
}

bool net_arp_known(uint32_t ip) {
    uint8_t mac[6];
    return arp_lookup(ip, mac);
}

// Send an ARP request or reply (`operation`) for `target_ip` to `dst_mac`.
// Called without net_lock.
static void arp_send(uint16_t operation, const uint8_t dst_mac[6], const uint8_t target_mac[6], uint32_t target_ip) {
    uint8_t frame[sizeof(struct eth_header) + sizeof(struct arp_packet)];
    struct eth_header *eth = (struct eth_header *)frame;
    struct arp_packet *arp = (struct arp_packet *)(frame + sizeof(*eth));

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, our_mac, 6);
    eth->ethertype = htons(ETHERTYPE_ARP);

    arp->hardware_type = htons(1);
    arp->protocol_type = htons(ETHERTYPE_IPV4);
    arp->hardware_len = 6;
    arp->protocol_len = 4;
    arp->operation = htons(operation);
    memcpy(arp->sender_mac, our_mac, 6);
    arp->sender_ip = htonl(local_ip);
    memcpy(arp->target_mac, target_mac, 6);
    arp->target_ip = htonl(target_ip);

    virtio_net_send(frame, sizeof(frame));
}

// A received ARP packet: learn the sender if it was addressed to us, and
// answer a request for our address
static void arp_input(const uint8_t *payload, size_t len) {
    if (len < sizeof(struct arp_packet)) { stats.dropped++; return; }
    const struct arp_packet *arp = (const struct arp_packet *)payload;

    if (ntohs(arp->hardware_type) != 1 || ntohs(arp->protocol_type) != ETHERTYPE_IPV4 ||
        arp->hardware_len != 6 || arp->protocol_len != 4) {
        stats.dropped++;
        return;
    }

    // Whoever is talking to us is worth remembering
    uint32_t sender_ip = ntohl(arp->sender_ip);
    if (local_ip == 0) return; // No address yet: we are nobody's target
    if (ntohl(arp->target_ip) == local_ip) arp_learn(sender_ip, arp->sender_mac);

    if (ntohs(arp->operation) == ARP_REQUEST && ntohl(arp->target_ip) == local_ip) {
        arp_send(ARP_REPLY, arp->sender_mac, arp->sender_mac, sender_ip);
        stats.arp_replies_sent++;
    }
}

// Find the hardware address to send to for `ip`, asking the network if
// necessary. May sleep. Returns 0, -EHOSTUNREACH or -EINTR.
static int arp_resolve(uint32_t ip, uint8_t mac[6], bool can_block) {
    if (arp_lookup(ip, mac)) return 0;
    if (!can_block) return -EHOSTUNREACH;

    static const uint8_t unknown[6] = { 0 };
    for (int attempt = 0; attempt < ARP_TRIES; attempt++) {
        arp_send(ARP_REQUEST, broadcast_mac, unknown, ip);

        uint64_t deadline = timer_ticks() + ARP_WAIT_TICKS;
        uint64_t flags = spinlock_acquire_irqsave(&net_lock);
        while (!arp_lookup_locked(ip, mac) && timer_ticks() < deadline) {
            if (sched_interrupted()) {
                spinlock_release_irqrestore(&net_lock, flags);
                return -EINTR;
            }
            sched_sleep(arp_cache, &net_lock, deadline);
        }
        spinlock_release_irqrestore(&net_lock, flags);
        if (arp_lookup(ip, mac)) return 0;
    }
    return -EHOSTUNREACH;
}

int net_set_config(const net_config_t *config) {
    if (!config) return -EINVAL;
    if (!up) return -ENETDOWN;

    if (config->ip != 0) {
        // A netmask is ones followed by zeros: its complement plus one is
        // then a power of two
        uint32_t host_bits = ~config->netmask;
        if ((host_bits & (host_bits + 1)) != 0 || config->netmask == 0) return -EINVAL;
        if ((config->ip >> 24) == 127 || (config->ip >> 28) >= 14) return -EINVAL;      // Loopback, multicast, reserved
        if ((config->ip & host_bits) == 0 || (config->ip & host_bits) == host_bits) return -EINVAL; // The network itself, or its broadcast address
        if (config->gateway != 0 && (config->gateway & config->netmask) != (config->ip & config->netmask)) return -EINVAL;
    }

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    local_ip = config->ip;
    local_netmask = config->ip ? config->netmask : 0;
    local_gateway = config->ip ? config->gateway : 0;
    local_dns = config->dns;
    memset(arp_cache, 0, sizeof(arp_cache)); // Neighbours of the old network
    spinlock_release_irqrestore(&net_lock, flags);
    sched_wakeup(arp_cache);
    return 0;
}

// --- IPv4 ----------------------------------------------------------------------

static void ipv4_input(const uint8_t *packet, size_t len, const uint8_t *src_mac);

// Send `len` bytes of `payload` as an IPv4 packet. `can_block` says whether
// the caller may sleep waiting for an ARP answer (the receive thread may not:
// it is the one that would process the answer).
static int ipv4_send(uint32_t dst, uint8_t protocol, const void *payload, size_t len, bool can_block) {
    if (!up) return -ENETDOWN;
    if (len > 1500 - sizeof(struct ipv4_header)) return -EMSGSIZE;

    uint8_t frame[ETH_FRAME_MAX];
    struct eth_header *eth = (struct eth_header *)frame;
    struct ipv4_header *ip = (struct ipv4_header *)(frame + sizeof(*eth));
    static uint16_t next_id = 1;

    ip->version_ihl = 0x45;                // IPv4, 5 words (20 bytes) of header
    ip->tos = 0;
    ip->total_length = htons((uint16_t)(sizeof(*ip) + len));
    ip->id = htons(next_id++);
    ip->flags_fragment = htons(0x4000);    // "Don't fragment"
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src = htonl(net_source_for(dst));
    ip->dst = htonl(dst);
    ip->checksum = htons(net_checksum(ip, sizeof(*ip)));
    memcpy(frame + sizeof(*eth) + sizeof(*ip), payload, len);

    uint32_t our_ip = local_ip, netmask = local_netmask;

    // To ourselves: no need to involve the card
    if ((our_ip != 0 && dst == our_ip) || (dst >> 24) == 127) {
        ipv4_input(frame + sizeof(*eth), sizeof(*ip) + len, our_mac);
        return 0;
    }

    bool broadcast = dst == NET_BROADCAST || (our_ip != 0 && dst == (our_ip | ~netmask));
    if (broadcast) {
        // To everyone on the wire: no address to look up
        memcpy(eth->dst, broadcast_mac, 6);
    } else if (our_ip == 0) {
        return -ENETDOWN; // Without an address of our own, nobody could answer
    } else {
        // On our network the frame goes to the machine itself; anything
        // else goes to the gateway, which forwards it
        uint32_t next_hop = ((dst & netmask) == (our_ip & netmask)) ? dst : local_gateway;
        if (next_hop == 0) return -EHOSTUNREACH; // Off our network, and no gateway
        int err = arp_resolve(next_hop, eth->dst, can_block);
        if (err != 0) return err;
    }

    memcpy(eth->src, our_mac, 6);
    eth->ethertype = htons(ETHERTYPE_IPV4);
    return virtio_net_send(frame, sizeof(*eth) + sizeof(*ip) + len) == 0 ? 0 : -ENETDOWN;
}

int net_ipv4_send(uint32_t dst, uint8_t protocol, const void *payload, size_t len, bool can_block) {
    return ipv4_send(dst, protocol, payload, len, can_block);
}

// A received ICMP message: answer an echo request (without waiting for
// ARP: this is the receive thread), or note the answer to one of our pings
static void icmp_input(uint32_t src, const uint8_t *data, size_t len) {
    if (len < sizeof(struct icmp_header) || net_checksum(data, len) != 0) { stats.dropped++; return; }
    const struct icmp_header *icmp = (const struct icmp_header *)data;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        // The reply is the request with the type changed and the checksum redone
        uint8_t reply[1500 - sizeof(struct ipv4_header)];
        if (len > sizeof(reply)) { stats.dropped++; return; }
        memcpy(reply, data, len);
        struct icmp_header *out = (struct icmp_header *)reply;
        out->type = ICMP_ECHO_REPLY;
        out->checksum = 0;
        out->checksum = htons(net_checksum(reply, len));
        if (ipv4_send(src, IP_PROTO_ICMP, reply, len, false) == 0) stats.echo_replies_sent++;
    } else if (icmp->type == ICMP_ECHO_REPLY) {
        uint64_t flags = spinlock_acquire_irqsave(&net_lock);
        for (size_t i = 0; i < sizeof(pings) / sizeof(pings[0]); i++) {
            if (pings[i].active && pings[i].id == ntohs(icmp->id) && pings[i].sequence == ntohs(icmp->sequence)) {
                pings[i].answered = true;
            }
        }
        spinlock_release_irqrestore(&net_lock, flags);
        sched_wakeup(pings);
    } else {
        stats.dropped++;
    }
}

// A received UDP datagram: queue it on the socket bound to its port, or
// count it as undeliverable (no socket, or its queue is full)
static void udp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len) {
    if (len < sizeof(struct udp_header)) { stats.dropped++; return; }
    const struct udp_header *udp = (const struct udp_header *)data;

    // The length field must agree with what actually arrived
    size_t udp_len = ntohs(udp->length);
    if (udp_len < sizeof(*udp) || udp_len > len) { stats.dropped++; return; }
    if (udp->checksum != 0 && udp_checksum(src, dst, data, udp_len) != 0) { stats.dropped++; return; }

    size_t payload_len = udp_len - sizeof(*udp);
    uint16_t port = ntohs(udp->dst_port);
    if (payload_len > UDP_PAYLOAD_MAX) { stats.dropped++; return; }

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    struct udp_socket *sock = NULL;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].open && sockets[i].port == port && port != 0) { sock = &sockets[i]; break; }
    }

    if (!sock) {
        stats.udp_no_socket++;
    } else if (sock->count == UDP_QUEUE_LEN) {
        stats.udp_queue_full++;
    } else {
        udp_datagram_t *slot = &sock->queue[(sock->head + sock->count) % UDP_QUEUE_LEN];
        slot->ip = src;
        slot->port = ntohs(udp->src_port);
        slot->len = (uint16_t)payload_len;
        memcpy(slot->data, data + sizeof(*udp), payload_len);
        sock->count++;
        stats.udp_delivered++;
    }
    spinlock_release_irqrestore(&net_lock, flags);

    if (sock) sched_wakeup(sock);
}

// A received IPv4 packet (header checked, fragments refused): hand its
// payload to ICMP, UDP or TCP if it is for us
static void ipv4_input(const uint8_t *packet, size_t len, const uint8_t *src_mac) {
    if (len < sizeof(struct ipv4_header)) { stats.dropped++; return; }
    const struct ipv4_header *ip = (const struct ipv4_header *)packet;

    size_t header_len = (size_t)(ip->version_ihl & 0x0F) * 4;
    size_t total_len = ntohs(ip->total_length);

    if ((ip->version_ihl >> 4) != 4 || header_len < sizeof(*ip) || header_len > len ||
        total_len < header_len || total_len > len ||           // Claims more than arrived
        net_checksum(packet, header_len) != 0) {
        stats.dropped++;
        return;
    }
    if (ntohs(ip->flags_fragment) & 0x3FFF) { stats.dropped++; return; } // A fragment: not supported
    // For us? Our address, a loopback address, or a broadcast. While we
    // have no address, whatever reached our card is for us: a DHCP server
    // answers to the address it is about to give out.
    uint32_t to = ntohl(ip->dst), our_ip = local_ip;
    bool for_us = our_ip == 0 || to == our_ip || (to >> 24) == 127 || to == NET_BROADCAST ||
                  to == (our_ip | ~local_netmask);
    if (!for_us) { stats.dropped++; return; }

    uint32_t src = ntohl(ip->src);
    // Remember how to reach the sender, so that a reply needs no ARP
    // exchange: if it is on our network. One further away was reached
    // through the gateway, and its frame carries the gateway's address;
    // learning that for every distant sender would push the gateway's own
    // entry (which replies need) out of the cache.
    if (our_ip == 0 || (src & local_netmask) == (our_ip & local_netmask)) arp_learn(src, src_mac);

    const uint8_t *payload = packet + header_len;
    size_t payload_len = total_len - header_len;

    if (ip->protocol == IP_PROTO_ICMP) icmp_input(src, payload, payload_len);
    else if (ip->protocol == IP_PROTO_UDP) udp_input(src, ntohl(ip->dst), payload, payload_len);
    else if (ip->protocol == IP_PROTO_TCP) tcp_input(src, ntohl(ip->dst), payload, payload_len);
    else stats.dropped++;
}

void net_handle_frame(const uint8_t *frame, size_t len) {
    stats.frames_in++;
    if (!frame || len < sizeof(struct eth_header)) { stats.dropped++; return; }
    const struct eth_header *eth = (const struct eth_header *)frame;

    // For us, or for everyone?
    if (memcmp(eth->dst, our_mac, 6) != 0 && memcmp(eth->dst, broadcast_mac, 6) != 0) { stats.dropped++; return; }

    const uint8_t *payload = frame + sizeof(*eth);
    size_t payload_len = len - sizeof(*eth);

    switch (ntohs(eth->ethertype)) {
        case ETHERTYPE_ARP:  arp_input(payload, payload_len); break;
        case ETHERTYPE_IPV4: ipv4_input(payload, payload_len, eth->src); break;
        default:             stats.dropped++; break;
    }
}

// --- Ping ------------------------------------------------------------------------

int net_ping(uint32_t ip, uint64_t timeout_ticks, uint64_t *rtt_ticks) {
    if (!up) return -ENETDOWN;

    // Claim a slot for matching the reply
    static uint16_t next_sequence = 1;
    ping_wait_t *wait = NULL;
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    for (size_t i = 0; i < sizeof(pings) / sizeof(pings[0]); i++) {
        if (!pings[i].active) { wait = &pings[i]; break; }
    }
    if (wait) {
        wait->active = true;
        wait->answered = false;
        wait->id = (uint16_t)get_random_u32();
        wait->sequence = next_sequence++;
    }
    spinlock_release_irqrestore(&net_lock, flags);
    if (!wait) return -EBUSY;

    struct {
        struct icmp_header header;
        uint8_t data[32];
    } __attribute__((packed)) request;
    request.header.type = ICMP_ECHO_REQUEST;
    request.header.code = 0;
    request.header.checksum = 0;
    request.header.id = htons(wait->id);
    request.header.sequence = htons(wait->sequence);
    for (size_t i = 0; i < sizeof(request.data); i++) request.data[i] = (uint8_t)('a' + i % 26);
    request.header.checksum = htons(net_checksum(&request, sizeof(request)));

    uint64_t start = timer_ticks();
    int result = ipv4_send(ip, IP_PROTO_ICMP, &request, sizeof(request), true);

    if (result == 0) {
        uint64_t deadline = start + timeout_ticks;
        flags = spinlock_acquire_irqsave(&net_lock);
        while (!wait->answered && timer_ticks() < deadline) {
            if (sched_interrupted()) { result = -EINTR; break; }
            sched_sleep(pings, &net_lock, deadline);
        }
        if (result == 0 && !wait->answered) result = -ETIMEDOUT;
        spinlock_release_irqrestore(&net_lock, flags);
    }

    if (result == 0 && rtt_ticks) *rtt_ticks = timer_ticks() - start;
    wait->active = false;
    return result;
}

// --- UDP sockets -------------------------------------------------------------------

// Is a socket bound to `port`? Caller holds net_lock.
static bool port_in_use(uint16_t port) {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].open && sockets[i].port == port) return true;
    }
    return false;
}

udp_socket_t *udp_open(void) {
    udp_datagram_t *queue = kmalloc(sizeof(udp_datagram_t) * UDP_QUEUE_LEN);
    if (!queue) return NULL;

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    struct udp_socket *sock = NULL;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!sockets[i].open) { sock = &sockets[i]; break; }
    }
    if (sock) {
        sock->open = true;
        sock->port = 0;
        sock->queue = queue;
        sock->head = 0;
        sock->count = 0;
    }
    spinlock_release_irqrestore(&net_lock, flags);

    if (!sock) kfree(queue);
    return sock;
}

int udp_bind(udp_socket_t *sock, uint16_t port) {
    if (!sock || !sock->open || port == 0 || sock->port != 0) return -EINVAL;

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    int result = port_in_use(port) ? -EADDRINUSE : 0;
    if (result == 0) sock->port = port;
    spinlock_release_irqrestore(&net_lock, flags);
    return result;
}

void udp_close(udp_socket_t *sock) {
    if (!sock || !sock->open) return;

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    udp_datagram_t *queue = sock->queue;
    sock->open = false;
    sock->port = 0;
    sock->queue = NULL;
    sock->count = 0;
    spinlock_release_irqrestore(&net_lock, flags);

    sched_wakeup(sock); // Anyone still waiting on it finds it closed
    kfree(queue);
}

int64_t udp_sendto(udp_socket_t *sock, uint32_t ip, uint16_t port, const void *data, size_t len) {
    if (!sock || !sock->open || !data) return -EINVAL;
    if (len > UDP_PAYLOAD_MAX) return -EMSGSIZE;

    // A socket that never bound gets a port now: the receiver needs
    // somewhere to send its answer
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    for (int tries = 0; sock->port == 0 && tries < 16384; tries++) {
        uint16_t candidate = next_ephemeral++;
        if (next_ephemeral == 0) next_ephemeral = EPHEMERAL_PORT_FIRST;
        if (candidate >= EPHEMERAL_PORT_FIRST && !port_in_use(candidate)) sock->port = candidate;
    }
    uint16_t src_port = sock->port;
    spinlock_release_irqrestore(&net_lock, flags);
    if (src_port == 0) return -EADDRINUSE;

    uint8_t packet[sizeof(struct udp_header) + UDP_PAYLOAD_MAX];
    struct udp_header *udp = (struct udp_header *)packet;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(port);
    udp->length = htons((uint16_t)(sizeof(*udp) + len));
    udp->checksum = 0;
    memcpy(packet + sizeof(*udp), data, len);

    uint16_t checksum = udp_checksum(net_source_for(ip), ip, packet, sizeof(*udp) + len);
    udp->checksum = htons(checksum == 0 ? 0xFFFF : checksum); // 0 on the wire means "none"

    int err = ipv4_send(ip, IP_PROTO_UDP, packet, sizeof(*udp) + len, true);
    return err == 0 ? (int64_t)len : err;
}

int64_t udp_recvfrom(udp_socket_t *sock, void *buf, size_t len, uint32_t *src_ip, uint16_t *src_port, uint64_t timeout_ticks) {
    if (!sock || !sock->open || !buf) return -EINVAL;
    if (sock->port == 0) return -EINVAL; // Nothing can arrive on a socket with no port

    uint64_t deadline = timeout_ticks ? timer_ticks() + timeout_ticks : 0;

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    while (sock->open && sock->count == 0) {
        if (sched_interrupted()) { spinlock_release_irqrestore(&net_lock, flags); return -EINTR; }
        if (deadline && timer_ticks() >= deadline) { spinlock_release_irqrestore(&net_lock, flags); return -ETIMEDOUT; }
        sched_sleep(sock, &net_lock, deadline);
    }
    if (!sock->open) {
        spinlock_release_irqrestore(&net_lock, flags);
        return -EBADF; // Closed while we waited
    }

    // `buf` is kernel memory (the system call copies to the program), so
    // the datagram goes straight there, under the lock
    const udp_datagram_t *front = &sock->queue[sock->head];
    size_t n = front->len < len ? front->len : len;
    memcpy(buf, front->data, n);
    if (src_ip) *src_ip = front->ip;
    if (src_port) *src_port = front->port;
    sock->head = (sock->head + 1) % UDP_QUEUE_LEN;
    sock->count--;
    spinlock_release_irqrestore(&net_lock, flags);
    return (int64_t)n;
}

// --- Start-up --------------------------------------------------------------------

// The receive thread: hand every arriving frame to the stack, sleep when
// there are none.
static void netd(void) {
    static uint8_t frame[ETH_FRAME_MAX];
    for (;;) {
        size_t len;
        while ((len = virtio_net_poll(frame, sizeof(frame))) > 0) {
            net_handle_frame(frame, len);
        }

        virtio_net_wait_rx();
    }
}

void net_init(void) {
    if (up || !virtio_net_present()) return;

    virtio_net_get_mac(our_mac);
    up = true;
    task_create("netd", netd);
    tcp_init();
    kprintf("net: 10.0.2.15/24, gateway 10.0.2.2 (the default; /bin/dhcp asks the network)\n");
}
