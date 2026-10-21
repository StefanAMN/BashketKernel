/**
 * @file ipfrag.h
 * @brief Reassembly of IPv4 datagrams that arrive in fragments
 *
 * A datagram larger than a link can carry is cut into fragments (RFC 791,
 * section 2.3): each carries the datagram's identification, the offset of
 * its data in the datagram (in units of 8 bytes) and a "more fragments"
 * flag, clear only on the last one. The receiver collects the fragments
 * whose source, destination, protocol and identification agree, and hands
 * the datagram on when every byte has arrived.
 *
 * What has not arrived yet is a list of holes (RFC 815): at first one hole
 * from 0 to "infinity", which the last fragment cuts down to the datagram's
 * length. Here the pieces received are kept in order of offset and the
 * holes are the gaps between them. A fragment must fall wholly inside one
 * hole:
 *  - one that repeats a piece already held exactly is a duplicate (the
 *    network sent it twice) and is ignored;
 *  - one that overlaps a piece any other way is refused, and the whole
 *    datagram with it. Overlapping fragments were the means of several
 *    attacks (data that differs depending on which piece wins, headers
 *    rewritten after a firewall checked them; RFC 1858, RFC 5722), and no
 *    honest sender produces them.
 *
 * Fragments are untrusted and cost memory, so there are limits: a datagram
 * may not end beyond IPV4_PAYLOAD_MAX ("ping of death"), at most
 * IPFRAG_SLOTS are in progress, together they hold at most IPFRAG_MEMORY
 * bytes (each piece charged IPFRAG_PIECE_OVERHEAD as well, so that tiny
 * fragments are not free), and one that is not complete IPFRAG_TIMEOUT_TICKS
 * after its first fragment arrived is thrown away. When a new fragment
 * needs a slot or memory that is not there, the oldest datagram in progress
 * is given up for it.
 *
 * A datagram given up for time whose first fragment arrived is reported
 * to the caller of ipfrag_expire with that fragment's header and first 8
 * bytes, for the ICMP "fragment reassembly time exceeded" message RFC 1122
 * (3.3.2) asks for.
 */

#ifndef IPFRAG_H
#define IPFRAG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pit.h"

#define IPV4_PAYLOAD_MAX 65515                 // 65535 (the total length field) - 20 (a header without options)
#define IPFRAG_SLOTS 8                         // Datagrams in progress at once
#define IPFRAG_MEMORY (256 * 1024)             // What they may hold together
#define IPFRAG_PIECE_OVERHEAD 32               // Charged per piece on top of its data
#define IPFRAG_TIMEOUT_TICKS (30 * TIMER_HZ)   // As Linux; RFC 791 suggests at least 15 s

typedef struct {
    uint64_t fragments;          // Fragments taken in
    uint64_t reassembled;        // Datagrams completed and handed on
    uint64_t duplicates;         // Fragments that repeated a piece already held (ignored)
    uint64_t refused;            // Datagrams given up: overlaps, contradictions, too long, malformed
    uint64_t timeouts;           // Datagrams given up: a piece never came
    uint64_t evicted;            // Datagrams given up to make room for another
} ipfrag_stats_t;

/**
 * @brief Take in one fragment of a datagram addressed to us.
 * @param src, dst, protocol, id  What identifies the datagram (RFC 791)
 * @param header The fragment's IPv4 header (20 bytes are kept from the
 *        first fragment, for the error ipfrag_expire may report)
 * @param offset Where the fragment's data goes, in bytes (the header's field times 8)
 * @param more The "more fragments" flag: clear on the last fragment
 * @param data, len The fragment's payload (copied; the caller keeps its buffer)
 * @param now The time in ticks: the datagram is given up IPFRAG_TIMEOUT_TICKS
 *        after its first fragment
 * @param total Set to the datagram's length when the result is not NULL
 * @param started Set to true if this fragment began a datagram (so that
 *        whoever runs ipfrag_expire can learn of the new deadline)
 * @return The whole datagram's payload, allocated with kmalloc (the caller
 *         frees it), when this fragment was the last piece missing; NULL
 *         otherwise (kept, ignored or refused: the statistics say which).
 *         Takes the reassembly lock; allocates; never sleeps.
 */
uint8_t *ipfrag_input(uint32_t src, uint32_t dst, uint8_t protocol, uint16_t id, size_t offset, bool more,
                      const void *header, const uint8_t *data, size_t len, uint64_t now, size_t *total, bool *started);

#define IPFRAG_QUOTE_LEN (20 + 8) // The first fragment's header and the first 8 bytes of its data

// Told of a datagram given up for time whose first fragment had arrived:
// `quote` is that fragment's header and first bytes (len of them)
typedef void (*ipfrag_report_t)(const uint8_t *quote, size_t len);

/**
 * @brief Give up every datagram whose time ran out by `now` (in ticks) and
 *        free what it held. Those whose first fragment had arrived are
 *        passed to `report` (unless it is NULL), after the lock is let go.
 * @return When the next one runs out (in ticks), or 0 if none is in progress.
 */
uint64_t ipfrag_expire(uint64_t now, ipfrag_report_t report);

/**
 * @brief The counters (read without a lock: each is a plain number).
 */
const ipfrag_stats_t *ipfrag_stats(void);

/**
 * @brief For tests: give datagrams that begin from now on `ticks` instead of
 *        IPFRAG_TIMEOUT_TICKS to complete (0 restores the default).
 */
void ipfrag_test_timeout(uint64_t ticks);

#endif
