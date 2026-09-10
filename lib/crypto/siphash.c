/**
 * @file siphash.c
 * @brief SipHash-2-4 Keyed Hash Function Implementation
 */

#include "siphash.h"

#define ROTL64(x, b) (((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND do { \
    v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0; v0 = ROTL64(v0, 32); \
    v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2; v2 = ROTL64(v2, 32); \
} while (0)

static uint64_t load64_le(const uint8_t *src) {
    return ((uint64_t)src[0]) |
           ((uint64_t)src[1] << 8) |
           ((uint64_t)src[2] << 16) |
           ((uint64_t)src[3] << 24) |
           ((uint64_t)src[4] << 32) |
           ((uint64_t)src[5] << 40) |
           ((uint64_t)src[6] << 48) |
           ((uint64_t)src[7] << 56);
}

uint64_t siphash24(const uint8_t key[16], const uint8_t *data, size_t len) {
    if (!key) return 0;

    uint64_t k0 = load64_le(key + 0);
    uint64_t k1 = load64_le(key + 8);

    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    const uint8_t *end = data + (len & ~7);
    const uint8_t *ptr = data;

    while (ptr < end) {
        uint64_t m = load64_le(ptr);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
        ptr += 8;
    }

    uint64_t b = (uint64_t)(len & 0xFF) << 56;
    switch (len & 7) {
        case 7: b |= (uint64_t)ptr[6] << 48; /* fall through */
        case 6: b |= (uint64_t)ptr[5] << 40; /* fall through */
        case 5: b |= (uint64_t)ptr[4] << 32; /* fall through */
        case 4: b |= (uint64_t)ptr[3] << 24; /* fall through */
        case 3: b |= (uint64_t)ptr[2] << 16; /* fall through */
        case 2: b |= (uint64_t)ptr[1] << 8; /* fall through */
        case 1: b |= (uint64_t)ptr[0];
        case 0: break;
    }

    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;

    v2 ^= 0xFF;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;

    return v0 ^ v1 ^ v2 ^ v3;
}
