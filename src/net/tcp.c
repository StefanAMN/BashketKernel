/**
 * @file tcp.c
 * @brief TCP (see tcp.h for what is and is not implemented)
 *
 * How the pieces fit:
 *
 *  - tcp_input() runs when a segment arrives. It only updates the socket
 *    (takes data in, notes what was acknowledged, changes state) and wakes
 *    whoever waits. It never sends: it can be called from inside a send (a
 *    segment to ourselves comes straight back in), and answering from there
 *    would nest sends without limit.
 *  - output() sends whatever the socket's state says is due: a SYN, data the
 *    window allows, a FIN, or a bare acknowledgement. Everything that is
 *    transmitted goes through it, so a retransmission is nothing special:
 *    the timer moves "next to send" back to "oldest unacknowledged" and
 *    calls it.
 *  - The tcpd thread runs the timers and calls output() for every socket
 *    whenever tcp_input() has left work (an acknowledgement to send) or
 *    time has passed.
 *
 * Sequence numbers count bytes, plus one for the SYN and one for the FIN,
 * and wrap around at 2^32: compare them only with the SEQ_ macros.
 */

#include "tcp.h"
#include "net.h"
#include "sched.h"
#include "spinlock.h"
#include "pit.h"
#include "heap.h"
#include "kstring.h"
#include "kprintf.h"
#include "kerrno.h"
#include "random.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_HEADER_LEN 20
#define TCP_OUR_MSS 1460            // 1500 (Ethernet payload) - 20 (IPv4) - 20 (TCP)
#define TCP_DEFAULT_MSS 536         // What to assume of a peer that does not say (RFC 9293, 3.7.1)

#define TCP_RTO_INITIAL 20          // Ticks (200 ms) before the first retransmission
#define TCP_RTO_MAX 300             // The back-off doubles up to 3 s
#define TCP_MAX_RETRIES 8           // Then the connection is given up
#define TCP_TIME_WAIT_TICKS 200     // 2 s in TIME_WAIT (stands in for twice the segment lifetime)
#define TCP_ORPHAN_TICKS 2000       // A closed socket may take 20 s to finish closing, no more
#define TCP_TIMER_TICKS 5           // How often tcpd looks at the timers

#define TCP_EPHEMERAL_FIRST 49152

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)

struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_offset;            // Header length in 32-bit words, in the high four bits
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

// Reverse the byte order of 16 and 32-bit values (the wire is big-endian)
static uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
// (See swap16)
static uint32_t swap32(uint32_t v) {
    return (v << 24) | ((v << 8) & 0x00FF0000) | ((v >> 8) & 0x0000FF00) | (v >> 24);
}

// The states of RFC 9293, figure 5
typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,       // We asked to connect; waiting for SYN+ACK
    TCP_SYN_RECEIVED,   // We answered a SYN; waiting for the ACK of ours
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,     // We closed first; our FIN is not yet acknowledged
    TCP_FIN_WAIT_2,     // ...it is; waiting for the other end's FIN
    TCP_CLOSE_WAIT,     // The other end closed first; we may still send
    TCP_CLOSING,        // Both closed at once; waiting for the ACK of our FIN
    TCP_LAST_ACK,       // The other end closed, then we did; waiting for that ACK
    TCP_TIME_WAIT,      // All done; lingering to answer a repeated FIN
} tcp_state_t;

struct tcp_socket {
    bool used;
    bool user_open;                 // Someone still holds it (a file descriptor)
    tcp_state_t state;
    int error;                      // Why the connection died (positive errno), 0 if it has not

    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;

    // Sending. The ring holds data that is not yet acknowledged: sent or not.
    uint32_t iss;                   // Our first sequence number (the SYN's)
    uint32_t snd_una;               // Oldest sequence number not yet acknowledged
    uint32_t snd_nxt;               // Next to send (moved back to snd_una to retransmit)
    uint32_t snd_max;               // Highest ever sent + 1
    uint32_t snd_wnd;               // How much the other end will take from snd_una on
    uint16_t mss;                   // Largest segment the other end takes
    bool syn_acked;
    bool fin_queued;                // A FIN follows the data in the ring
    uint8_t *tx;
    size_t tx_head, tx_len;

    // Receiving. The ring holds data not yet read by the owner.
    uint32_t irs;                   // The other end's first sequence number
    uint32_t rcv_nxt;               // Next sequence number expected
    bool peer_fin;                  // The other end has finished sending
    bool ack_pending;               // Something arrived that must be acknowledged
    uint8_t *rx;
    size_t rx_head, rx_len;

    // Timers, in timer ticks (0: not running)
    uint64_t rto_deadline;          // Retransmit if nothing is acknowledged by then
    uint32_t rto;                   // Current timeout
    uint32_t retries;
    uint64_t reap_deadline;         // Free the socket then, whatever state it is in

    // Only one CPU at a time sends for a socket: two doing it at once would
    // put their segments on the wire in either order
    bool sending;
    bool send_again;                // Someone else wanted to send meanwhile

    // A connection made by a listening socket and not yet taken by tcp_accept
    struct tcp_socket *listener;
    int backlog;                    // Of a listening socket
};

// A reset to send in answer to a segment that belongs to no connection
typedef struct {
    bool used;
    uint32_t ip;
    uint16_t local_port, remote_port;
    uint32_t seq, ack;
    uint8_t flags;
} pending_reset_t;

static struct tcp_socket sockets[TCP_MAX_SOCKETS];
static pending_reset_t resets[8];
static tcp_stats_t stats;
static uint16_t next_ephemeral = TCP_EPHEMERAL_FIRST;
static bool work_pending = false;   // tcp_input left something for tcpd to send
static uint32_t drop_every = 0, drop_counter = 0;

// One lock for all of the above. Never held across net_ipv4_send().
static spinlock_t tcp_lock = SPINLOCK_RANKED("tcp", LOCK_RANK_NET + 1);

const tcp_stats_t *tcp_stats(void) { return &stats; }

void tcp_test_drop_every(uint32_t n) {
    uint64_t flags = spinlock_acquire_irqsave(&tcp_lock);
    drop_every = n;
    drop_counter = 12345; // The same sequence every time: a failure can be repeated
    spinlock_release_irqrestore(&tcp_lock, flags);
}

size_t tcp_sockets_in_use(void) {
    size_t n = 0;
    uint64_t flags = spinlock_acquire_irqsave(&tcp_lock);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) if (sockets[i].used) n++;
    spinlock_release_irqrestore(&tcp_lock, flags);
    return n;
}

// --- Helpers (callers hold tcp_lock) -------------------------------------------

// Tell tcpd there is something to send. Caller holds tcp_lock.
static void kick_tcpd(void) {
    work_pending = true;
    sched_wakeup(&work_pending);
}

// Give back a socket's buffers and its slot. Caller holds tcp_lock.
static void free_socket(struct tcp_socket *sock) {
    kfree(sock->tx);
    kfree(sock->rx);
    memset(sock, 0, sizeof(*sock));
}

// The connection is over, now, whatever state it was in
static void abort_connection(struct tcp_socket *sock, int error) {
    sock->error = error;
    sock->state = TCP_CLOSED;
    sock->rto_deadline = 0;
    sock->ack_pending = false;
    sched_wakeup(sock);
    if (sock->listener) sched_wakeup(sock->listener);

    // Nobody will ever ask about it: a connection never accepted, or one
    // whose owner has already closed it
    if (sock->listener || !sock->user_open) free_socket(sock);
}

// Room left in the receive ring: what we offer the other end as window
static size_t rx_free(const struct tcp_socket *sock) {
    return TCP_BUFFER_SIZE - sock->rx_len;
}

// Is any socket on local port `port`? Caller holds tcp_lock.
static bool port_in_use(uint16_t port) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].local_port == port) return true;
    }
    return false;
}

// A free slot set up as a closed socket with both rings allocated, or NULL
// (all in use, or no memory). Caller holds tcp_lock.
static struct tcp_socket *alloc_socket(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        struct tcp_socket *sock = &sockets[i];
        if (sock->used) continue;

        memset(sock, 0, sizeof(*sock));
        sock->tx = kmalloc(TCP_BUFFER_SIZE);
        sock->rx = kmalloc(TCP_BUFFER_SIZE);
        if (!sock->tx || !sock->rx) {
            kfree(sock->tx);
            kfree(sock->rx);
            memset(sock, 0, sizeof(*sock));
            return NULL;
        }
        sock->used = true;
        kick_tcpd(); // It may be asleep with no timers to run
        sock->state = TCP_CLOSED;
        sock->mss = TCP_DEFAULT_MSS;
        sock->rto = TCP_RTO_INITIAL;
        return sock;
    }
    return NULL;
}

// Choose our first sequence number, at random. Caller holds tcp_lock.
static void start_sequence(struct tcp_socket *sock) {
    // An unpredictable first sequence number makes it hard for someone who
    // can not see the connection to forge segments for it
    sock->iss = get_random_u32();
    sock->snd_una = sock->iss;
    sock->snd_nxt = sock->iss;
    sock->snd_max = sock->iss;
}

// Leave a reset for tcpd to send (input never sends; see the top). Caller
// holds tcp_lock.
static void queue_reset(uint32_t ip, uint16_t local_port, uint16_t remote_port, uint32_t seq, uint32_t ack, uint8_t flags) {
    for (size_t i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
        if (resets[i].used) continue;
        resets[i] = (pending_reset_t){ true, ip, local_port, remote_port, seq, ack, flags };
        kick_tcpd();
        return;
    }
    // Full: the sender will try again and get its answer then
}

// A reset in answer to a segment we will have nothing to do with (RFC 9293,
// 3.10.7.1): it must look acceptable to the sender, so it takes its sequence
// number from the segment's acknowledgement, or acknowledges all of it
static void reset_for_segment(uint32_t ip, const struct tcp_header *tcp, size_t data_len) {
    if (tcp->flags & TCP_RST) return; // Never answer a reset with a reset
    uint16_t local_port = swap16(tcp->dst_port), remote_port = swap16(tcp->src_port);

    if (tcp->flags & TCP_ACK) {
        queue_reset(ip, local_port, remote_port, swap32(tcp->ack), 0, TCP_RST);
    } else {
        uint32_t consumed = (uint32_t)data_len + ((tcp->flags & TCP_SYN) ? 1 : 0) + ((tcp->flags & TCP_FIN) ? 1 : 0);
        queue_reset(ip, local_port, remote_port, 0, swap32(tcp->seq) + consumed, TCP_RST | TCP_ACK);
    }
}

// --- Sending ---------------------------------------------------------------------

// Build one segment and hand it to the IP layer. Called without the lock.
// `can_block`: the caller may sleep while the address of the next hop is
// looked up (ARP). The timer thread may not: every connection waits on it.
// It only ever sends to machines we have already exchanged segments with,
// or repeats a send that a caller who could wait has made.
static void transmit(uint32_t ip, uint16_t local_port, uint16_t remote_port, uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t window, const uint8_t *data, size_t len, bool can_block) {
    uint8_t segment[TCP_HEADER_LEN + 4 + TCP_OUR_MSS];
    struct tcp_header *tcp = (struct tcp_header *)segment;
    size_t header_len = TCP_HEADER_LEN;

    tcp->src_port = swap16(local_port);
    tcp->dst_port = swap16(remote_port);
    tcp->seq = swap32(seq);
    tcp->ack = swap32(ack);
    tcp->flags = flags;
    tcp->window = swap16(window);
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (flags & TCP_SYN) {
        // The one option we use: kind 2, length 4, the largest segment we take
        segment[header_len++] = 2;
        segment[header_len++] = 4;
        segment[header_len++] = (uint8_t)(TCP_OUR_MSS >> 8);
        segment[header_len++] = (uint8_t)(TCP_OUR_MSS & 0xFF);
    }
    tcp->data_offset = (uint8_t)((header_len / 4) << 4);

    if (len > 0) memcpy(segment + header_len, data, len);
    size_t total = header_len + len;
    tcp->checksum = swap16(net_transport_checksum(IP_PROTO_TCP, net_source_for(ip), ip, segment, total));

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    // One in drop_every, chosen by a fixed pseudo-random sequence rather
    // than every n-th: a regular pattern can fall into step with the
    // protocol and lose the same segment of every retransmission round
    bool drop = false;
    if (drop_every != 0) {
        drop_counter = drop_counter * 1103515245u + 12345u;
        drop = ((drop_counter >> 16) % drop_every) == 0;
    }
    if (drop) stats.dropped_for_test++;
    else stats.segments_out++;
    if (flags & TCP_RST) stats.resets_sent++;
    spinlock_release_irqrestore(&tcp_lock, irq);

    if (!drop) net_ipv4_send(ip, IP_PROTO_TCP, segment, total, can_block);
}

// Send everything that is due on this socket, one segment after another.
// Called without the lock, by output() only.
static void send_what_is_due(struct tcp_socket *sock, bool can_block) {
    for (;;) {
        uint8_t data[TCP_OUR_MSS];
        size_t len = 0;
        uint8_t flags = 0;

        uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
        if (!sock->used || sock->state == TCP_CLOSED || sock->state == TCP_LISTEN) {
            spinlock_release_irqrestore(&tcp_lock, irq);
            return;
        }

        uint32_t seq = sock->snd_nxt;

        if (!sock->syn_acked) {
            // Still opening: the only thing to send is the SYN, once per
            // (re)transmission
            if (sock->snd_nxt == sock->iss) {
                flags = TCP_SYN | (sock->state == TCP_SYN_RECEIVED ? TCP_ACK : 0);
                sock->snd_nxt = sock->iss + 1;
            }
        } else {
            // Where snd_nxt is in the ring: the ring starts at snd_una
            uint32_t offset = sock->snd_nxt - sock->snd_una;
            size_t unsent = sock->tx_len > offset ? sock->tx_len - offset : 0;
            uint32_t in_flight = offset;
            size_t usable = sock->snd_wnd > in_flight ? sock->snd_wnd - in_flight : 0;

            // The other end has no room and nothing is on its way that would
            // make it say when it has: send one byte to get an answer
            if (sock->snd_wnd == 0 && in_flight == 0 && unsent > 0) usable = 1;

            len = unsent;
            if (len > usable) len = usable;
            if (len > sock->mss) len = sock->mss;
            for (size_t i = 0; i < len; i++) {
                data[i] = sock->tx[(sock->tx_head + offset + i) % TCP_BUFFER_SIZE];
            }
            if (len > 0) {
                flags = TCP_ACK | TCP_PSH;
                sock->snd_nxt += (uint32_t)len;
            }

            // The FIN goes after the last byte, and takes a sequence number
            if (sock->fin_queued && offset + len == sock->tx_len && sock->snd_nxt == sock->snd_una + sock->tx_len) {
                flags |= TCP_FIN | TCP_ACK;
                sock->snd_nxt++;
            }

            if (flags == 0 && sock->ack_pending) flags = TCP_ACK;
        }

        if (flags == 0) {
            spinlock_release_irqrestore(&tcp_lock, irq);
            return;
        }

        bool takes_sequence_space = len > 0 || (flags & (TCP_SYN | TCP_FIN));
        if (takes_sequence_space) {
            if (SEQ_GT(sock->snd_nxt, sock->snd_max)) sock->snd_max = sock->snd_nxt;
            if (sock->rto_deadline == 0) sock->rto_deadline = timer_ticks() + sock->rto;
        }
        if (flags & TCP_ACK) sock->ack_pending = false;

        uint32_t ip = sock->remote_ip;
        uint16_t local_port = sock->local_port, remote_port = sock->remote_port;
        uint32_t ack = (flags & TCP_ACK) ? sock->rcv_nxt : 0;
        size_t room = rx_free(sock);
        uint16_t window = (uint16_t)(room > 0xFFFF ? 0xFFFF : room);
        spinlock_release_irqrestore(&tcp_lock, irq);

        transmit(ip, local_port, remote_port, seq, ack, flags, window, data, len, can_block);

        if (!takes_sequence_space) return; // A bare acknowledgement: nothing more can be due
    }
}

// Send everything that is due on this socket. Called without the lock.
static void output(struct tcp_socket *sock, bool can_block) {
    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    if (!sock->used || sock->sending) {
        // Whoever is sending will look again when done, and see what we saw
        if (sock->used) sock->send_again = true;
        spinlock_release_irqrestore(&tcp_lock, irq);
        return;
    }
    sock->sending = true;
    spinlock_release_irqrestore(&tcp_lock, irq);

    for (;;) {
        send_what_is_due(sock, can_block);

        irq = spinlock_acquire_irqsave(&tcp_lock);
        bool again = sock->send_again;
        sock->send_again = false;
        if (!again) sock->sending = false;
        spinlock_release_irqrestore(&tcp_lock, irq);
        if (!again) return;
    }
}

// --- Receiving -------------------------------------------------------------------

// The largest segment the other end takes, from the options of its SYN
// (bounds-checked: they come from the network), at most our own
static uint16_t mss_from_options(const uint8_t *options, size_t len) {
    for (size_t i = 0; i < len; ) {
        uint8_t kind = options[i];
        if (kind == 0) break;                       // End of the list
        if (kind == 1) { i++; continue; }           // Padding
        if (i + 1 >= len) break;
        uint8_t option_len = options[i + 1];
        if (option_len < 2 || i + option_len > len) break; // Malformed: stop
        if (kind == 2 && option_len == 4) {
            uint16_t mss = (uint16_t)((options[i + 2] << 8) | options[i + 3]);
            if (mss >= 64) return mss < TCP_OUR_MSS ? mss : TCP_OUR_MSS;
        }
        i += option_len;
    }
    return TCP_DEFAULT_MSS;
}

// The connection these addresses and ports belong to, or NULL. Caller
// holds tcp_lock.
static struct tcp_socket *find_connection(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        struct tcp_socket *s = &sockets[i];
        if (s->used && s->state != TCP_LISTEN && s->state != TCP_CLOSED && s->local_port == local_port &&
            s->remote_ip == remote_ip && s->remote_port == remote_port) return s;
    }
    return NULL;
}

// The socket listening on `local_port`, or NULL. Caller holds tcp_lock.
static struct tcp_socket *find_listener(uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        struct tcp_socket *s = &sockets[i];
        if (s->used && s->state == TCP_LISTEN && s->local_port == local_port) return s;
    }
    return NULL;
}

// A SYN for a listening socket: start a connection of its own
static void accept_syn(struct tcp_socket *listener, uint32_t src, const struct tcp_header *tcp,
                       const uint8_t *options, size_t options_len) {
    int waiting = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].listener == listener) waiting++;
    }
    if (waiting >= listener->backlog) return; // No room: stay silent, the sender will try again

    struct tcp_socket *sock = alloc_socket();
    if (!sock) return;

    sock->listener = listener;
    sock->state = TCP_SYN_RECEIVED;
    sock->local_port = listener->local_port;
    sock->remote_ip = src;
    sock->remote_port = swap16(tcp->src_port);
    sock->irs = swap32(tcp->seq);
    sock->rcv_nxt = sock->irs + 1;                  // The SYN counts as one
    sock->snd_wnd = swap16(tcp->window);
    sock->mss = mss_from_options(options, options_len);
    start_sequence(sock);
    kick_tcpd();                                    // To send the SYN+ACK
}

// The other end acknowledged everything up to `ack`
static void process_ack(struct tcp_socket *sock, uint32_t ack) {
    uint32_t acked = ack - sock->snd_una;
    size_t data_acked = acked < sock->tx_len ? acked : sock->tx_len;
    bool fin_acked = sock->fin_queued && acked > sock->tx_len; // One more than the data: our FIN

    sock->tx_head = (sock->tx_head + data_acked) % TCP_BUFFER_SIZE;
    sock->tx_len -= data_acked;
    sock->snd_una = ack;
    if (SEQ_LT(sock->snd_nxt, sock->snd_una)) sock->snd_nxt = sock->snd_una;

    // Progress: start timing afresh, for whatever is still outstanding
    sock->rto = TCP_RTO_INITIAL;
    sock->rto_deadline = (sock->snd_una == sock->snd_max) ? 0 : timer_ticks() + sock->rto;

    if (fin_acked) {
        sock->fin_queued = false;
        if (sock->state == TCP_FIN_WAIT_1) {
            sock->state = TCP_FIN_WAIT_2;
        } else if (sock->state == TCP_CLOSING) {
            sock->state = TCP_TIME_WAIT;
            sock->reap_deadline = timer_ticks() + TCP_TIME_WAIT_TICKS;
        } else if (sock->state == TCP_LAST_ACK) {
            sock->state = TCP_CLOSED;
            if (!sock->user_open) { free_socket(sock); return; }
        }
    }
    sched_wakeup(sock); // A writer may be waiting for room
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *segment, size_t len) {
    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    stats.segments_in++;

    const struct tcp_header *tcp = (const struct tcp_header *)segment;
    size_t header_len = len >= TCP_HEADER_LEN ? (size_t)(tcp->data_offset >> 4) * 4 : 0;
    if (len < TCP_HEADER_LEN || header_len < TCP_HEADER_LEN || header_len > len ||
        net_transport_checksum(IP_PROTO_TCP, src, dst, segment, len) != 0) {
        stats.bad_segments++;
        goto done;
    }

    const uint8_t *data = segment + header_len;
    size_t data_len = len - header_len;
    uint32_t seq = swap32(tcp->seq), ack = swap32(tcp->ack);
    uint8_t flags = tcp->flags;
    uint16_t local_port = swap16(tcp->dst_port), remote_port = swap16(tcp->src_port);

    struct tcp_socket *sock = find_connection(local_port, src, remote_port);

    if (!sock) {
        struct tcp_socket *listener = find_listener(local_port);
        if (listener && (flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) {
            accept_syn(listener, src, tcp, segment + TCP_HEADER_LEN, header_len - TCP_HEADER_LEN);
        } else {
            reset_for_segment(src, tcp, data_len); // Nobody here by that name
        }
        goto done;
    }

    if (flags & TCP_RST) stats.resets_received++;

    // --- We asked to connect and this should be the answer ---
    if (sock->state == TCP_SYN_SENT) {
        bool ack_ok = (flags & TCP_ACK) && ack == sock->iss + 1;
        if ((flags & TCP_ACK) && !ack_ok) {
            reset_for_segment(src, tcp, data_len);
        } else if (flags & TCP_RST) {
            if (ack_ok) abort_connection(sock, ECONNREFUSED); // Nobody listening there
        } else if ((flags & TCP_SYN) && ack_ok) {
            sock->irs = seq;
            sock->rcv_nxt = seq + 1;
            sock->snd_una = ack;
            sock->syn_acked = true;
            sock->snd_wnd = swap16(tcp->window);
            sock->mss = mss_from_options(segment + TCP_HEADER_LEN, header_len - TCP_HEADER_LEN);
            sock->rto = TCP_RTO_INITIAL;
            sock->rto_deadline = 0;
            sock->retries = 0;
            sock->state = TCP_ESTABLISHED;
            sock->ack_pending = true;               // The third step of the handshake
            sched_wakeup(sock);
            kick_tcpd();
        }
        goto done;
    }

    // --- Every other state: the connection has sequence numbers on both sides ---

    if (flags & TCP_RST) {
        // Only a reset that is exactly where we expect the other end to be
        // is believed: anyone can send one
        if (seq == sock->rcv_nxt) abort_connection(sock, sock->state == TCP_SYN_RECEIVED ? ECONNREFUSED : ECONNRESET);
        goto done;
    }

    if (flags & TCP_SYN) {
        // The other end did not get our SYN+ACK and is asking again
        if (sock->state == TCP_SYN_RECEIVED && seq == sock->irs) {
            sock->snd_nxt = sock->iss;
            kick_tcpd();
        }
        goto done;
    }

    if (!(flags & TCP_ACK)) goto done; // Everything after the first SYN carries one

    if (sock->state == TCP_SYN_RECEIVED) {
        if (ack != sock->iss + 1) {
            reset_for_segment(src, tcp, data_len);
            goto done;
        }
        sock->snd_una = ack;
        sock->syn_acked = true;
        sock->rto = TCP_RTO_INITIAL;
        sock->rto_deadline = 0;
        sock->retries = 0;
        sock->state = TCP_ESTABLISHED;
        if (sock->listener) sched_wakeup(sock->listener); // tcp_accept may be waiting
    } else if (SEQ_GT(ack, sock->snd_max)) {
        // Acknowledges something we never sent: say where we really are
        sock->ack_pending = true;
        kick_tcpd();
        goto done;
    } else if (SEQ_GT(ack, sock->snd_una)) {
        process_ack(sock, ack);
        if (!sock->used) goto done; // That was the last thing it was waiting for
    }

    // Any acceptable segment shows the other end is alive and tells us its window
    sock->retries = 0;
    bool window_opened = sock->snd_wnd == 0 && swap16(tcp->window) > 0;
    sock->snd_wnd = swap16(tcp->window);
    if (window_opened && sock->snd_una != sock->snd_max) {
        // What is outstanding is the probe byte, which the other end had no
        // room for. Send from there again now rather than when the (by now
        // long) timer runs out.
        sock->snd_nxt = sock->snd_una;
        sock->rto = TCP_RTO_INITIAL;
        sock->rto_deadline = timer_ticks() + sock->rto;
    }
    if (window_opened || sock->tx_len > 0 || sock->fin_queued) kick_tcpd();

    // --- What it carries ---
    bool fin = (flags & TCP_FIN) != 0;
    if (data_len == 0 && !fin) goto done;

    // Part of it may be a repeat of what we already have: skip that part
    if (SEQ_LT(seq, sock->rcv_nxt)) {
        uint32_t repeated = sock->rcv_nxt - seq;
        if (repeated >= data_len + (fin ? 1 : 0)) {
            stats.out_of_order++;                   // All of it is old: acknowledge again
            sock->ack_pending = true;
            kick_tcpd();
            goto done;
        }
        if (repeated > data_len) repeated = (uint32_t)data_len;
        data += repeated;
        data_len -= repeated;
        seq += repeated;
    }
    if (seq != sock->rcv_nxt) {
        // A gap: something before this was lost. It is not kept (tcp.h);
        // saying again what we expect makes the sender's timer bring it back.
        stats.out_of_order++;
        sock->ack_pending = true;
        kick_tcpd();
        goto done;
    }

    bool receiving = sock->state == TCP_ESTABLISHED || sock->state == TCP_FIN_WAIT_1 || sock->state == TCP_FIN_WAIT_2;
    size_t accepted = 0;
    if (receiving) {
        if (!sock->user_open && !sock->listener) {
            accepted = data_len;                    // Its owner has closed it: take it and drop it
        } else {
            // (A connection still waiting for tcp_accept keeps what arrives:
            // the other end may start talking as soon as it is connected.)
            accepted = data_len < rx_free(sock) ? data_len : rx_free(sock);
            size_t tail = (sock->rx_head + sock->rx_len) % TCP_BUFFER_SIZE;
            for (size_t i = 0; i < accepted; i++) sock->rx[(tail + i) % TCP_BUFFER_SIZE] = data[i];
            sock->rx_len += accepted;
        }
        sock->rcv_nxt += (uint32_t)accepted;
    }

    // The FIN comes after the data: it only counts once all of that is in
    if (fin && accepted == data_len && seq + (uint32_t)data_len == sock->rcv_nxt) {
        if (receiving) {
            sock->rcv_nxt++;
            sock->peer_fin = true;
            if (sock->state == TCP_ESTABLISHED) {
                sock->state = TCP_CLOSE_WAIT;
            } else if (sock->state == TCP_FIN_WAIT_1) {
                sock->state = TCP_CLOSING;          // Our FIN is still unacknowledged
            } else {
                sock->state = TCP_TIME_WAIT;
                sock->reap_deadline = timer_ticks() + TCP_TIME_WAIT_TICKS;
            }
        }
        // (In any later state it is a repeat of a FIN already counted.)
    }

    sock->ack_pending = true;
    sched_wakeup(sock);
    kick_tcpd();

done:
    spinlock_release_irqrestore(&tcp_lock, irq);
}

// --- Timers ----------------------------------------------------------------------

// Caller holds tcp_lock
static void run_timers(struct tcp_socket *sock, uint64_t now) {
    if (sock->reap_deadline != 0 && now >= sock->reap_deadline) {
        // TIME_WAIT is over, or a closed socket has taken too long to finish
        if (sock->user_open) {
            sock->reap_deadline = 0;
            sock->state = TCP_CLOSED;
            sock->rto_deadline = 0;
        } else {
            free_socket(sock);
        }
        return;
    }

    if (sock->rto_deadline != 0 && now >= sock->rto_deadline) {
        if (sock->snd_una == sock->snd_max) {
            sock->rto_deadline = 0;                 // Nothing outstanding after all
            return;
        }
        if (++sock->retries > TCP_MAX_RETRIES) {
            abort_connection(sock, ETIMEDOUT);
            return;
        }

        // Go back to the oldest thing not acknowledged and send from there
        // again; wait twice as long for the answer this time
        stats.retransmissions++;
        sock->snd_nxt = sock->syn_acked ? sock->snd_una : sock->iss;
        sock->rto = sock->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : sock->rto * 2;
        sock->rto_deadline = now + sock->rto;
    }
}

// The TCP thread: runs the timers, sends the queued resets, and calls
// output() for every socket, whenever woken and every TCP_TIMER_TICKS
// while there is a connection
static void tcpd(void) {
    for (;;) {
        uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
        if (!work_pending) {
            // Timers only run while there is a connection to run them for:
            // with none, sleep until there is one (see alloc_socket)
            bool any = false;
            for (int i = 0; i < TCP_MAX_SOCKETS && !any; i++) any = sockets[i].used;
            sched_sleep_uninterruptible(&work_pending, &tcp_lock, any ? timer_ticks() + TCP_TIMER_TICKS : 0);
        }
        work_pending = false;

        uint64_t now = timer_ticks();
        pending_reset_t to_send[sizeof(resets) / sizeof(resets[0])];
        memcpy(to_send, resets, sizeof(resets));
        memset(resets, 0, sizeof(resets));
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (sockets[i].used) run_timers(&sockets[i], now);
        }
        spinlock_release_irqrestore(&tcp_lock, irq);

        for (size_t i = 0; i < sizeof(to_send) / sizeof(to_send[0]); i++) {
            if (!to_send[i].used) continue;
            transmit(to_send[i].ip, to_send[i].local_port, to_send[i].remote_port,
                     to_send[i].seq, to_send[i].ack, to_send[i].flags, 0, NULL, 0, false);
        }
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) output(&sockets[i], false);
    }
}

void tcp_init(void) {
    task_create("tcpd", tcpd);
}

// --- What a socket's owner calls ---------------------------------------------------

tcp_socket_t *tcp_open(void) {
    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    struct tcp_socket *sock = alloc_socket();
    if (sock) sock->user_open = true;
    spinlock_release_irqrestore(&tcp_lock, irq);
    return sock;
}

int tcp_bind(tcp_socket_t *sock, uint16_t port) {
    if (!sock || port == 0) return -EINVAL;

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    int result = 0;
    if (sock->state != TCP_CLOSED || sock->local_port != 0 || sock->error) result = -EINVAL;
    else if (port_in_use(port)) result = -EADDRINUSE;
    else sock->local_port = port;
    spinlock_release_irqrestore(&tcp_lock, irq);
    return result;
}

int tcp_listen(tcp_socket_t *sock, int backlog) {
    if (!sock) return -EINVAL;
    if (backlog < 1) backlog = 1;
    if (backlog > TCP_BACKLOG_MAX) backlog = TCP_BACKLOG_MAX;

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    int result = 0;
    if (sock->state != TCP_CLOSED || sock->local_port == 0 || sock->error) {
        result = -EINVAL; // Not bound, or already in use for something else
    } else {
        sock->state = TCP_LISTEN;
        sock->backlog = backlog;
    }
    spinlock_release_irqrestore(&tcp_lock, irq);
    return result;
}

int tcp_connect(tcp_socket_t *sock, uint32_t ip, uint16_t port) {
    if (!sock || ip == 0 || port == 0) return -EINVAL;
    if (!net_up()) return -ENETDOWN;

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    if (sock->state != TCP_CLOSED || sock->error) {
        spinlock_release_irqrestore(&tcp_lock, irq);
        return -EISCONN;
    }
    for (int tries = 0; sock->local_port == 0 && tries < 16384; tries++) {
        uint16_t candidate = next_ephemeral++;
        if (next_ephemeral == 0) next_ephemeral = TCP_EPHEMERAL_FIRST;
        if (candidate >= TCP_EPHEMERAL_FIRST && !port_in_use(candidate)) sock->local_port = candidate;
    }
    if (sock->local_port == 0) {
        spinlock_release_irqrestore(&tcp_lock, irq);
        return -EADDRINUSE;
    }
    sock->remote_ip = ip;
    sock->remote_port = port;
    start_sequence(sock);
    sock->state = TCP_SYN_SENT;
    spinlock_release_irqrestore(&tcp_lock, irq);

    output(sock, true); // The SYN

    irq = spinlock_acquire_irqsave(&tcp_lock);
    int result = 0;
    while (sock->state == TCP_SYN_SENT) {
        if (sched_interrupted()) {
            // Give up: the socket can not be used for another attempt
            sock->error = EINTR;
            sock->state = TCP_CLOSED;
            sock->rto_deadline = 0;
            result = -EINTR;
            break;
        }
        sched_sleep(sock, &tcp_lock, 0);
    }
    if (result == 0 && sock->error) result = -sock->error;
    spinlock_release_irqrestore(&tcp_lock, irq);
    return result;
}

tcp_socket_t *tcp_accept(tcp_socket_t *sock, int *error) {
    int err = 0;
    struct tcp_socket *found = NULL;

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    while (sock) {
        if (sock->state != TCP_LISTEN) { err = -EINVAL; break; }

        for (int i = 0; i < TCP_MAX_SOCKETS && !found; i++) {
            struct tcp_socket *s = &sockets[i];
            // Complete, even if the other end has already said all it had to
            if (s->used && s->listener == sock && (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)) found = s;
        }
        if (found) {
            found->listener = NULL;
            found->user_open = true;
            break;
        }
        if (sched_interrupted()) { err = -EINTR; break; }
        sched_sleep(sock, &tcp_lock, 0);
    }
    if (!sock) err = -EINVAL;
    spinlock_release_irqrestore(&tcp_lock, irq);

    if (error) *error = err;
    return found;
}

int64_t tcp_read(tcp_socket_t *sock, void *buf, size_t len) {
    if (!sock || !buf) return -EINVAL;
    if (len == 0) return 0;

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    while (sock->rx_len == 0) {
        int64_t result = 1;
        if (sock->error) result = -sock->error;
        else if (sock->peer_fin) result = 0;                // End of the stream
        else if (sock->state == TCP_CLOSED || sock->state == TCP_LISTEN) result = -ENOTCONN;
        else if (sched_interrupted()) result = -EINTR;
        if (result <= 0) {
            spinlock_release_irqrestore(&tcp_lock, irq);
            return result;
        }
        sched_sleep(sock, &tcp_lock, 0);
    }

    size_t room_before = rx_free(sock);
    size_t n = len < sock->rx_len ? len : sock->rx_len;
    uint8_t *out = buf;
    for (size_t i = 0; i < n; i++) out[i] = sock->rx[(sock->rx_head + i) % TCP_BUFFER_SIZE];
    sock->rx_head = (sock->rx_head + n) % TCP_BUFFER_SIZE;
    sock->rx_len -= n;

    // If the other end had been told there was (nearly) no room, tell it
    // there is again; otherwise it would have to find out by probing
    if (room_before < sock->mss && rx_free(sock) >= sock->mss) {
        sock->ack_pending = true;
        kick_tcpd();
    }
    spinlock_release_irqrestore(&tcp_lock, irq);
    return (int64_t)n;
}

int64_t tcp_write(tcp_socket_t *sock, const void *buf, size_t len) {
    if (!sock || !buf) return -EINVAL;

    const uint8_t *in = buf;
    size_t done = 0;

    while (done < len) {
        uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
        int64_t failure = 0;
        for (;;) {
            if (sock->error) failure = (sock->error == ECONNRESET) ? -EPIPE : -sock->error;
            else if (sock->state != TCP_ESTABLISHED && sock->state != TCP_CLOSE_WAIT) {
                failure = (sock->state == TCP_CLOSED || sock->state == TCP_LISTEN) ? -ENOTCONN : -EPIPE;
            }
            if (failure || sock->tx_len < TCP_BUFFER_SIZE) break;
            if (sched_interrupted()) { failure = -EINTR; break; }
            sched_sleep(sock, &tcp_lock, 0);   // The buffer is full: wait for acknowledgements
        }
        if (failure) {
            spinlock_release_irqrestore(&tcp_lock, irq);
            return done > 0 ? (int64_t)done : failure;
        }

        size_t room = TCP_BUFFER_SIZE - sock->tx_len;
        size_t n = len - done < room ? len - done : room;
        size_t tail = (sock->tx_head + sock->tx_len) % TCP_BUFFER_SIZE;
        for (size_t i = 0; i < n; i++) sock->tx[(tail + i) % TCP_BUFFER_SIZE] = in[done + i];
        sock->tx_len += n;
        done += n;
        spinlock_release_irqrestore(&tcp_lock, irq);

        output(sock, true);
    }
    return (int64_t)done;
}

void tcp_close(tcp_socket_t *sock) {
    if (!sock) return;

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    if (!sock->used || !sock->user_open) {
        spinlock_release_irqrestore(&tcp_lock, irq);
        return;
    }
    sock->user_open = false;
    sock->rx_len = 0; // Nobody will read it

    switch (sock->state) {
        case TCP_LISTEN:
            // Connections nobody accepted are closed like any other; ones
            // still half open are forgotten
            for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
                struct tcp_socket *s = &sockets[i];
                if (!s->used || s->listener != sock) continue;
                s->listener = NULL;
                if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
                    s->state = (s->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
                    s->fin_queued = true;
                    s->reap_deadline = timer_ticks() + TCP_ORPHAN_TICKS;
                } else {
                    free_socket(s);
                }
            }
            free_socket(sock);
            break;

        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
            // Say we are done once what is queued has gone; the rest of the
            // close happens in the background, for a limited time
            sock->state = (sock->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
            sock->fin_queued = true;
            sock->reap_deadline = timer_ticks() + TCP_ORPHAN_TICKS;
            break;

        case TCP_FIN_WAIT_1:
        case TCP_FIN_WAIT_2:
        case TCP_CLOSING:
        case TCP_LAST_ACK:
            // Already closing (tcp_shutdown): let it finish, within limits
            sock->reap_deadline = timer_ticks() + TCP_ORPHAN_TICKS;
            break;

        case TCP_TIME_WAIT:
            break; // Its timer frees it

        default:
            free_socket(sock); // Closed, failed, or never got as far as connecting
            break;
    }
    kick_tcpd();
    spinlock_release_irqrestore(&tcp_lock, irq);
}

int tcp_shutdown(tcp_socket_t *sock) {
    if (!sock) return -EINVAL;

    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    int result = 0;
    if (sock->state == TCP_ESTABLISHED) sock->state = TCP_FIN_WAIT_1;
    else if (sock->state == TCP_CLOSE_WAIT) sock->state = TCP_LAST_ACK;
    else result = -ENOTCONN;
    if (result == 0) {
        sock->fin_queued = true;
        kick_tcpd();
    }
    spinlock_release_irqrestore(&tcp_lock, irq);
    return result;
}

void tcp_peer(tcp_socket_t *sock, uint32_t *ip, uint16_t *port) {
    uint64_t irq = spinlock_acquire_irqsave(&tcp_lock);
    if (ip) *ip = sock ? sock->remote_ip : 0;
    if (port) *port = sock ? sock->remote_port : 0;
    spinlock_release_irqrestore(&tcp_lock, irq);
}
