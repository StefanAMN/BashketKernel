/**
 * @file ipfrag.c
 * @brief Reassembly of IPv4 fragments (see ipfrag.h for the rules)
 *
 * Memory is allocated and freed outside the lock: a fragment's piece is
 * allocated before the lock is taken, and whatever is given up (a refused
 * piece, a datagram that timed out or was evicted, the pieces of a finished
 * datagram) is unlinked under the lock and freed after it.
 */

#include "ipfrag.h"
#include "heap.h"
#include "kstring.h"
#include "spinlock.h"

// One fragment's data, at its place in the datagram
typedef struct piece {
    struct piece *next;            // The next piece by offset
    uint32_t offset;               // In bytes from the start of the datagram
    uint32_t len;
    uint8_t data[];
} piece_t;

// A datagram in progress
typedef struct {
    bool used;
    uint32_t src, dst;
    uint16_t id;
    uint8_t protocol;
    piece_t *pieces;               // In order of offset, never overlapping
    size_t received;               // Bytes held (the pieces' lengths added up)
    size_t total;                  // The datagram's length, once the last fragment has said (0: not yet)
    size_t cost;                   // What it is charged against IPFRAG_MEMORY
    uint64_t deadline;             // Given up at this tick
    size_t quote_len;              // The first fragment's header and first bytes, once it has come
    uint8_t quote[IPFRAG_QUOTE_LEN];
} reassembly_t;

static reassembly_t slots[IPFRAG_SLOTS];
static size_t memory_used;
static ipfrag_stats_t stats;
static uint64_t timeout_ticks = IPFRAG_TIMEOUT_TICKS;
static spinlock_t frag_lock = SPINLOCK_RANKED("ipfrag", LOCK_RANK_NET);

const ipfrag_stats_t *ipfrag_stats(void) { return &stats; }

void ipfrag_test_timeout(uint64_t ticks) { timeout_ticks = ticks ? ticks : IPFRAG_TIMEOUT_TICKS; }

// Empty a slot, adding its pieces to *garbage to be freed after the lock.
// Caller holds frag_lock.
static void release(reassembly_t *r, piece_t **garbage) {
    piece_t *p = r->pieces;
    while (p) {
        piece_t *next = p->next;
        p->next = *garbage;
        *garbage = p;
        p = next;
    }
    memory_used -= r->cost;
    memset(r, 0, sizeof(*r));
}

// Free a list of pieces collected under the lock (call without it)
static void free_pieces(piece_t *p) {
    while (p) {
        piece_t *next = p->next;
        kfree(p);
        p = next;
    }
}

// The datagram in progress oldest in time, other than `keep` (which may be
// NULL); NULL if there is none. Caller holds frag_lock.
static reassembly_t *oldest_except(const reassembly_t *keep) {
    reassembly_t *oldest = NULL;
    for (int i = 0; i < IPFRAG_SLOTS; i++) {
        if (slots[i].used && &slots[i] != keep && (!oldest || slots[i].deadline < oldest->deadline)) {
            oldest = &slots[i];
        }
    }
    return oldest;
}

// Copy the pieces of a complete datagram into one buffer of `total` bytes
// (call without the lock: the pieces have been unlinked). NULL if there is
// no memory for it.
static uint8_t *assemble(const piece_t *pieces, size_t total) {
    uint8_t *whole = kmalloc(total);
    if (!whole) return NULL;
    for (const piece_t *p = pieces; p; p = p->next) memcpy(whole + p->offset, p->data, p->len);
    return whole;
}

uint8_t *ipfrag_input(uint32_t src, uint32_t dst, uint8_t protocol, uint16_t id, size_t offset, bool more,
                      const void *header, const uint8_t *data, size_t len, uint64_t now, size_t *total, bool *started) {
    stats.fragments++;
    *started = false;

    // Every fragment but the last carries a multiple of 8 bytes (the unit
    // of the offset field), and at least one; and no datagram is longer
    // than the total length field can say
    bool malformed = (more && (len == 0 || len % 8 != 0)) || offset + len > IPV4_PAYLOAD_MAX;

    // Allocated before the lock; freed after it if it is not kept
    piece_t *piece = malformed ? NULL : kmalloc(sizeof(piece_t) + len);
    if (piece) {
        piece->next = NULL;
        piece->offset = (uint32_t)offset;
        piece->len = (uint32_t)len;
        memcpy(piece->data, data, len);
    }
    size_t cost = IPFRAG_PIECE_OVERHEAD + len;
    piece_t *garbage = NULL;
    piece_t *complete = NULL;
    size_t complete_len = 0;

    uint64_t flags = spinlock_acquire_irqsave(&frag_lock);

    reassembly_t *r = NULL;
    for (int i = 0; i < IPFRAG_SLOTS && !r; i++) {
        reassembly_t *s = &slots[i];
        if (s->used && s->src == src && s->dst == dst && s->protocol == protocol && s->id == id) r = s;
    }

    if (malformed) {
        // A datagram one of whose fragments is impossible goes with it
        stats.refused++;
        if (r) release(r, &garbage);
        goto out;
    }
    if (!piece) goto out; // No memory for it: as if it had been lost on the way

    if (!r) {
        for (int i = 0; i < IPFRAG_SLOTS && !r; i++) {
            if (!slots[i].used) r = &slots[i];
        }
        if (!r) {
            r = oldest_except(NULL);
            release(r, &garbage);
            stats.evicted++;
        }
        r->used = true;
        r->src = src;
        r->dst = dst;
        r->protocol = protocol;
        r->id = id;
        r->deadline = now + timeout_ticks;
        *started = true;
    }

    // Find the gap the piece goes into: after `prev`, before `next`
    size_t end = offset + len;
    piece_t *prev = NULL, *next = r->pieces;
    while (next && next->offset < offset) {
        prev = next;
        next = next->next;
    }

    if (next && next->offset == offset && next->len == len && (more || r->total == end)) {
        stats.duplicates++; // Exactly a piece already held
        goto out;
    }

    bool overlaps = (prev && prev->offset + prev->len > offset) || (next && next->offset < end);
    // The last fragment says where the datagram ends: nothing may lie beyond
    // that, and no other fragment may say otherwise
    bool contradicts = (r->total != 0 && end > r->total) || (!more && r->total != 0 && end != r->total);
    if (!more && r->total == 0) {
        for (piece_t *p = next; p; p = p->next) {
            if (p->offset + p->len > end) contradicts = true;
        }
    }
    if (overlaps || contradicts) {
        stats.refused++;
        release(r, &garbage);
        goto out;
    }

    // Room for it: give up older datagrams first; if it is this one that
    // is too large, give it up instead
    while (memory_used + cost > IPFRAG_MEMORY) {
        reassembly_t *victim = oldest_except(r);
        if (!victim) {
            stats.refused++;
            release(r, &garbage);
            goto out;
        }
        release(victim, &garbage);
        stats.evicted++;
    }

    piece->next = next;
    if (prev) prev->next = piece;
    else r->pieces = piece;
    piece = NULL;                                  // The datagram has it now
    r->received += len;
    r->cost += cost;
    if (offset == 0) {
        // For the error report if the rest never comes (options left out:
        // the header is quoted as 20 bytes)
        memcpy(r->quote, header, 20);
        r->quote[0] = 0x45;
        size_t n = len < 8 ? len : 8;
        memcpy(r->quote + 20, data, n);
        r->quote_len = 20 + n;
    }
    memory_used += cost;
    if (!more) r->total = end;

    // Pieces never overlap, so "as many bytes as the datagram is long" means
    // there are no holes left
    if (r->total != 0 && r->received == r->total) {
        complete = r->pieces;
        complete_len = r->total;
        r->pieces = NULL;
        memory_used -= r->cost;
        memset(r, 0, sizeof(*r));
    }

out:
    spinlock_release_irqrestore(&frag_lock, flags);
    kfree(piece);                                  // (NULL if it was kept)
    free_pieces(garbage);
    if (!complete) return NULL;

    uint8_t *whole = assemble(complete, complete_len);
    free_pieces(complete);
    if (!whole) {
        stats.refused++;
        return NULL;
    }
    stats.reassembled++;
    *total = complete_len;
    return whole;
}

uint64_t ipfrag_expire(uint64_t now, ipfrag_report_t report) {
    piece_t *garbage = NULL;
    uint64_t next_deadline = 0;
    uint8_t quotes[IPFRAG_SLOTS][IPFRAG_QUOTE_LEN];   // Reported after the lock
    size_t quote_lens[IPFRAG_SLOTS], reports = 0;

    uint64_t flags = spinlock_acquire_irqsave(&frag_lock);
    for (int i = 0; i < IPFRAG_SLOTS; i++) {
        reassembly_t *r = &slots[i];
        if (!r->used) continue;
        if (r->deadline <= now) {
            if (r->quote_len > 0) {
                memcpy(quotes[reports], r->quote, r->quote_len);
                quote_lens[reports++] = r->quote_len;
            }
            release(r, &garbage);
            stats.timeouts++;
        } else if (next_deadline == 0 || r->deadline < next_deadline) {
            next_deadline = r->deadline;
        }
    }
    spinlock_release_irqrestore(&frag_lock, flags);

    free_pieces(garbage);
    for (size_t i = 0; report && i < reports; i++) report(quotes[i], quote_lens[i]);
    return next_deadline;
}
