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
#include "poll.h"
#include "ipfrag.h"
#include "timer.h"

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
#define ARP_STALE_TICKS 6000       // An answer older than 60 s is asked for again (a machine
                                   // may have changed its card, a gateway been replaced)

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
    uint64_t last_used;            // For replacement when the cache is full
    uint64_t learned;              // When the machine last said this itself (ticks)
} arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

// A ping in progress: the reply handler matches on id and sequence
typedef struct {
    bool active;
    bool answered;
    uint16_t id, sequence;
} ping_wait_t;
static ping_wait_t pings[4];

// A received datagram, allocated at its own size and queued on its socket
typedef struct udp_datagram {
    struct udp_datagram *next;
    uint32_t ip;                   // Where it came from
    uint16_t port;
    uint16_t len;
    uint8_t data[];
} udp_datagram_t;
_Static_assert(sizeof(udp_datagram_t) == UDP_DATAGRAM_OVERHEAD, "the overhead charged is the header's size");

struct udp_socket {
    bool open;
    uint16_t port;                 // 0: not bound
    udp_datagram_t *head, *tail;   // Received, oldest first
    size_t count;                  // How many
    size_t bytes;                  // What they cost (UDP_DATAGRAM_OVERHEAD + payload each)
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
//
// pseudo_header_sum is the sum of that header alone, for a segment of `len`
// bytes, to which the segment's own bytes are then added
static uint32_t pseudo_header_sum(uint8_t protocol, uint32_t src_ip, uint32_t dst_ip, size_t len) {
    uint8_t pseudo[12];
    uint32_t src = htonl(src_ip), dst = htonl(dst_ip);
    uint16_t length = htons((uint16_t)len);
    memcpy(pseudo + 0, &src, 4);
    memcpy(pseudo + 4, &dst, 4);
    pseudo[8] = 0;
    pseudo[9] = protocol;
    memcpy(pseudo + 10, &length, 2);
    return checksum_add(0, pseudo, sizeof(pseudo));
}

uint16_t net_transport_checksum(uint8_t protocol, uint32_t src_ip, uint32_t dst_ip, const void *segment, size_t len) {
    return checksum_finish(checksum_add(pseudo_header_sum(protocol, src_ip, dst_ip, len), segment, len));
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
    slot->last_used = slot->learned = timer_ticks();
    spinlock_release_irqrestore(&net_lock, flags);

    sched_wakeup(arp_cache); // Someone may be waiting for exactly this answer
}

// The hardware address for `ip`, if known. *stale (if not NULL) says
// whether the answer is old enough to be asked for again. Caller holds
// net_lock.
static bool arp_lookup_locked(uint32_t ip, uint8_t mac[6], bool *stale) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            uint64_t now = timer_ticks();
            arp_cache[i].last_used = now;
            if (stale) *stale = now - arp_cache[i].learned > ARP_STALE_TICKS;
            return true;
        }
    }
    return false;
}

#ifdef KERNEL_TESTS
bool net_arp_age(uint32_t ip, uint64_t ticks) {
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    bool found = false;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip) {
            arp_cache[i].learned -= ticks;
            found = true;
        }
    }
    spinlock_release_irqrestore(&net_lock, flags);
    return found;
}
#endif

// arp_lookup_locked, taking net_lock
static bool arp_lookup(uint32_t ip, uint8_t mac[6], bool *stale) {
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    bool found = arp_lookup_locked(ip, mac, stale);
    spinlock_release_irqrestore(&net_lock, flags);
    return found;
}

bool net_arp_known(uint32_t ip) {
    uint8_t mac[6];
    return arp_lookup(ip, mac, NULL);
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
    if (operation == ARP_REQUEST) stats.arp_requests_sent++;   // (Approximate, like the others: no lock)

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
//
// An answer older than ARP_STALE_TICKS is asked for again. A caller that
// may wait waits for the new answer (and gets EHOSTUNREACH if none comes:
// the machine has gone). One that may not (the receive thread) uses the old
// answer, and the question it sends brings a new one for next time.
static int arp_resolve(uint32_t ip, uint8_t mac[6], bool can_block) {
    static const uint8_t unknown[6] = { 0 };
    bool stale = false;
    if (arp_lookup(ip, mac, &stale) && !stale) return 0;
    if (!can_block) {
        if (!stale) return -EHOSTUNREACH;                  // Never known
        arp_send(ARP_REQUEST, broadcast_mac, unknown, ip);
        return 0;
    }

    for (int attempt = 0; attempt < ARP_TRIES; attempt++) {
        arp_send(ARP_REQUEST, broadcast_mac, unknown, ip);

        uint64_t deadline = timer_ticks() + ARP_WAIT_TICKS;
        uint64_t flags = spinlock_acquire_irqsave(&net_lock);
        // (An old answer is not the answer: a new one resets its age)
        while (!(arp_lookup_locked(ip, mac, &stale) && !stale) && timer_ticks() < deadline) {
            if (sched_interrupted()) {
                spinlock_release_irqrestore(&net_lock, flags);
                return -EINTR;
            }
            sched_sleep(arp_cache, &net_lock, deadline);
        }
        spinlock_release_irqrestore(&net_lock, flags);
        if (arp_lookup(ip, mac, &stale) && !stale) return 0;
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

#define IPV4_SEND_WAIT_TICKS 20 // How long a burst of fragments may wait for the card

// Hand one frame to the card. When its send buffers are all in flight and
// the caller may sleep, wait a tick at a time (up to IPV4_SEND_WAIT_TICKS)
// for the card to finish some: a fragmented datagram is a burst of frames,
// longer than the card has buffers for, and losing one fragment loses all.
static int send_frame(const void *frame, size_t len, bool can_block) {
    for (int waited = 0; virtio_net_send(frame, len) != 0; waited++) {
        if (!can_block || waited >= IPV4_SEND_WAIT_TICKS) return -ENETDOWN;
        if (!sched_sleep_ticks(1)) return -EINTR;
    }
    return 0;
}

// Send `head_len` bytes of `head` followed by `body_len` bytes of `body` as
// the payload of one IPv4 datagram (two parts, so that a transport header
// need not be copied in front of the data first). A datagram that does not
// fit in one frame goes as fragments (RFC 791, 2.3): each carries the same
// identification, the offset of its data in units of 8 bytes, and "more
// fragments" on all but the last. One that fits keeps "don't fragment", so
// that a router with a smaller link refuses it instead of cutting it up
// (TCP sizes its segments to fit). `can_block` says whether the caller may
// sleep waiting for an ARP answer or for the card (the receive thread may
// not: it is the one that would process the answer).
static int ipv4_send_parts(uint32_t dst, uint8_t protocol, const void *head, size_t head_len,
                           const void *body, size_t body_len, bool can_block) {
    if (!up) return -ENETDOWN;
    size_t len = head_len + body_len;
    if (len > IPV4_PAYLOAD_MAX) return -EMSGSIZE;

    uint8_t frame[ETH_FRAME_MAX];
    struct eth_header *eth = (struct eth_header *)frame;
    struct ipv4_header *ip = (struct ipv4_header *)(frame + sizeof(*eth));
    // Shared by every CPU: two datagrams given the same number would be
    // mixed up by a receiver reassembling both
    static uint16_t next_id = 1;
    uint16_t id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);

    uint32_t our_ip = local_ip, netmask = local_netmask;
    bool loopback = (our_ip != 0 && dst == our_ip) || (dst >> 24) == 127;

    if (loopback) {
        // To ourselves: no need to involve the card
    } else if (dst == NET_BROADCAST || (our_ip != 0 && dst == (our_ip | ~netmask))) {
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

    // The largest multiple of 8 that fits after the header: every fragment
    // but the last carries exactly that much
    const size_t per_fragment = (1500 - sizeof(*ip)) & ~(size_t)7;
    bool fragmented = len > 1500 - sizeof(*ip);
    if (fragmented) stats.fragmented_sent++;

    size_t offset = 0;
    do {
        size_t n = len - offset < per_fragment ? len - offset : per_fragment;
        bool more = offset + n < len;

        ip->version_ihl = 0x45;            // IPv4, 5 words (20 bytes) of header
        ip->tos = 0;
        ip->total_length = htons((uint16_t)(sizeof(*ip) + n));
        ip->id = htons(id);
        ip->flags_fragment = htons(fragmented ? (uint16_t)((more ? 0x2000 : 0) | (offset / 8))
                                              : 0x4000); // "More fragments" and the offset, or "don't fragment"
        ip->ttl = 64;
        ip->protocol = protocol;
        ip->checksum = 0;
        ip->src = htonl(net_source_for(dst));
        ip->dst = htonl(dst);
        ip->checksum = htons(net_checksum(ip, sizeof(*ip)));

        // This fragment's share of head and body
        uint8_t *out = frame + sizeof(*eth) + sizeof(*ip);
        size_t from_head = offset < head_len ? head_len - offset : 0;
        if (from_head > n) from_head = n;
        if (from_head) memcpy(out, (const uint8_t *)head + offset, from_head);
        if (n > from_head) memcpy(out + from_head, (const uint8_t *)body + (offset + from_head - head_len), n - from_head);

        if (loopback) {
            ipv4_input(frame + sizeof(*eth), sizeof(*ip) + n, our_mac);
        } else {
            int err = send_frame(frame, sizeof(*eth) + sizeof(*ip) + n, can_block);
            if (err != 0) return err;
        }
        offset += n;
    } while (offset < len);
    return 0;
}

// Send `len` bytes of `payload` as one IPv4 datagram (ipv4_send_parts)
static int ipv4_send(uint32_t dst, uint8_t protocol, const void *payload, size_t len, bool can_block) {
    return ipv4_send_parts(dst, protocol, payload, len, NULL, 0, can_block);
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
        // The reply is the request with the type changed and the checksum
        // redone (as long as the request was: up to 64 KiB, if it came in
        // fragments, and goes back the same way)
        uint8_t *reply = kmalloc(len);
        if (!reply) { stats.dropped++; return; }
        memcpy(reply, data, len);
        struct icmp_header *out = (struct icmp_header *)reply;
        out->type = ICMP_ECHO_REPLY;
        out->checksum = 0;
        out->checksum = htons(net_checksum(reply, len));
        if (ipv4_send(src, IP_PROTO_ICMP, reply, len, false) == 0) stats.echo_replies_sent++;
        kfree(reply);
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

    // Allocated before the lock (and freed after it if nobody takes it)
    size_t cost = UDP_DATAGRAM_OVERHEAD + payload_len;
    udp_datagram_t *datagram = kmalloc(cost);
    if (!datagram) { stats.dropped++; return; }
    datagram->next = NULL;
    datagram->ip = src;
    datagram->port = ntohs(udp->src_port);
    datagram->len = (uint16_t)payload_len;
    memcpy(datagram->data, data + sizeof(*udp), payload_len);

    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    struct udp_socket *sock = NULL;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].open && sockets[i].port == port && port != 0) { sock = &sockets[i]; break; }
    }

    if (!sock) {
        stats.udp_no_socket++;
    } else if (sock->bytes + cost > UDP_QUEUE_BYTES) {
        stats.udp_queue_full++;
    } else {
        if (sock->tail) sock->tail->next = datagram;
        else sock->head = datagram;
        sock->tail = datagram;
        sock->count++;
        sock->bytes += cost;
        datagram = NULL;                                   // The socket has it now
        stats.udp_delivered++;
    }
    spinlock_release_irqrestore(&net_lock, flags);
    kfree(datagram);                                       // (NULL if it was queued)

    if (sock) {
        sched_wakeup(sock);
        poll_notify();
    }
}

// Hand a datagram's payload to ICMP, UDP or TCP
static void deliver(uint8_t protocol, uint32_t src, uint32_t dst, const uint8_t *payload, size_t len) {
    if (protocol == IP_PROTO_ICMP) icmp_input(src, payload, len);
    else if (protocol == IP_PROTO_UDP) udp_input(src, dst, payload, len);
    else if (protocol == IP_PROTO_TCP) tcp_input(src, dst, payload, len);
    else stats.dropped++;
}

// A received IPv4 packet: check its header, and if it is for us hand its
// payload on, or to reassembly first if it is a fragment
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

    // Bits 15..13 of the field: reserved, "don't fragment", "more
    // fragments"; bits 12..0: the offset in units of 8 bytes. A packet with
    // more to come or a non-zero offset is a fragment.
    uint16_t fragment = ntohs(ip->flags_fragment);
    bool more = fragment & 0x2000;
    size_t offset = (size_t)(fragment & 0x1FFF) * 8;
    if (!more && offset == 0) {
        deliver(ip->protocol, src, to, payload, payload_len);
        return;
    }

    size_t whole_len = 0;
    bool started = false;
    uint8_t *whole = ipfrag_input(src, to, ip->protocol, ntohs(ip->id), offset, more,
                                  payload, payload_len, timer_ticks(), &whole_len, &started);
    // A new datagram brings a new deadline, which the receive thread must
    // wait for (it is the one that gives up incomplete datagrams)
    if (started) virtio_net_wake_rx();
    if (whole) {
        deliver(ip->protocol, src, to, whole, whole_len);
        kfree(whole);
    }
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
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    struct udp_socket *sock = NULL;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!sockets[i].open) { sock = &sockets[i]; break; }
    }
    if (sock) {
        sock->open = true;
        sock->port = 0;
        sock->head = sock->tail = NULL;
        sock->count = sock->bytes = 0;
    }
    spinlock_release_irqrestore(&net_lock, flags);
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
    udp_datagram_t *queue = sock->head;
    sock->open = false;
    sock->port = 0;
    sock->head = sock->tail = NULL;
    sock->count = sock->bytes = 0;
    spinlock_release_irqrestore(&net_lock, flags);

    sched_wakeup(sock); // Anyone still waiting on it finds it closed
    while (queue) {     // What was never read
        udp_datagram_t *next = queue->next;
        kfree(queue);
        queue = next;
    }
}

int udp_poll(udp_socket_t *sock) {
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    int events = (sock->open && sock->count > 0) ? POLLIN : 0;
    spinlock_release_irqrestore(&net_lock, flags);
    return events | POLLOUT; // Sending never waits for room (a datagram is sent or dropped)
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

    // The header goes in front of the data as the datagram is cut into
    // frames, so the data (up to 64 KiB) is not copied an extra time here
    struct udp_header udp;
    udp.src_port = htons(src_port);
    udp.dst_port = htons(port);
    udp.length = htons((uint16_t)(sizeof(udp) + len));
    udp.checksum = 0;

    // The header's length is even, so the data's sum can follow it
    uint32_t sum = pseudo_header_sum(IP_PROTO_UDP, net_source_for(ip), ip, sizeof(udp) + len);
    uint16_t checksum = checksum_finish(checksum_add(checksum_add(sum, &udp, sizeof(udp)), data, len));
    udp.checksum = htons(checksum == 0 ? 0xFFFF : checksum); // 0 on the wire means "none"

    int err = ipv4_send_parts(ip, IP_PROTO_UDP, &udp, sizeof(udp), data, len, true);
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
    udp_datagram_t *front = sock->head;
    size_t n = front->len < len ? front->len : len;
    memcpy(buf, front->data, n);
    if (src_ip) *src_ip = front->ip;
    if (src_port) *src_port = front->port;
    sock->head = front->next;
    if (!sock->head) sock->tail = NULL;
    sock->count--;
    sock->bytes -= UDP_DATAGRAM_OVERHEAD + front->len;
    spinlock_release_irqrestore(&net_lock, flags);
    kfree(front);
    return (int64_t)n;
}

// --- Start-up --------------------------------------------------------------------

// The receive thread: hand every arriving frame to the stack, give up
// fragmented datagrams that took too long, sleep when there is nothing to do.
static void netd(void) {
    static uint8_t frame[ETH_FRAME_MAX];
    for (;;) {
        size_t len;
        while ((len = virtio_net_poll(frame, sizeof(frame))) > 0) {
            net_handle_frame(frame, len);
        }

        // Datagrams whose fragments did not all come in time are given up
        // here, so the thread wakes for the next such deadline too
        virtio_net_wait_rx(ipfrag_expire(timer_ticks()));
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
