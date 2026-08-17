/**
 * @file chacha20.c
 * @brief The ChaCha20 stream cipher (RFC 8439, section 2)
 */

#include "chacha20.h"
#include "../string.h"

// The cipher's state: constants, key, block counter and nonce, sixteen
// 32-bit words (RFC 8439, 2.3)
typedef struct {
    uint32_t state[16];
} chacha20_ctx_t;

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QUARTERROUND(a, b, c, d) do { \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7);  \
} while(0)

// A little-endian 32-bit word at `src`: read it, and write one
static uint32_t load32_le(const uint8_t *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

// (See load32_le)
static void store32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val & 0xFF);
    dst[1] = (uint8_t)((val >> 8) & 0xFF);
    dst[2] = (uint8_t)((val >> 16) & 0xFF);
    dst[3] = (uint8_t)((val >> 24) & 0xFF);
}

// Set up the state for `key`, `nonce` and the first block number
static void chacha20_init(chacha20_ctx_t *ctx, const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
    if (!ctx || !key || !nonce) return;
    // Constants "expand 32-byte k"
    ctx->state[0]  = 0x61707865;
    ctx->state[1]  = 0x3320646e;
    ctx->state[2]  = 0x79622d32;
    ctx->state[3]  = 0x6b206574;
    // Key (256 bits)
    ctx->state[4]  = load32_le(key + 0);
    ctx->state[5]  = load32_le(key + 4);
    ctx->state[6]  = load32_le(key + 8);
    ctx->state[7]  = load32_le(key + 12);
    ctx->state[8]  = load32_le(key + 16);
    ctx->state[9]  = load32_le(key + 20);
    ctx->state[10] = load32_le(key + 24);
    ctx->state[11] = load32_le(key + 28);
    // Block Counter
    ctx->state[12] = counter;
    // Nonce (96 bits)
    ctx->state[13] = load32_le(nonce + 0);
    ctx->state[14] = load32_le(nonce + 4);
    ctx->state[15] = load32_le(nonce + 8);
}

// One 64-byte block of keystream for the state's current block number
// (twenty rounds, then the input added back: RFC 8439, 2.3)
static void chacha20_block(const chacha20_ctx_t *ctx, uint8_t stream[64]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = ctx->state[i];

    for (int i = 0; i < 10; i++) {
        // Column rounds
        QUARTERROUND(x[0], x[4], x[8],  x[12]);
        QUARTERROUND(x[1], x[5], x[9],  x[13]);
        QUARTERROUND(x[2], x[6], x[10], x[14]);
        QUARTERROUND(x[3], x[7], x[11], x[15]);
        // Diagonal rounds
        QUARTERROUND(x[0], x[5], x[10], x[15]);
        QUARTERROUND(x[1], x[6], x[11], x[12]);
        QUARTERROUND(x[2], x[7], x[8],  x[13]);
        QUARTERROUND(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++) {
        x[i] += ctx->state[i];
        store32_le(stream + i * 4, x[i]);
    }
}

// XOR `len` bytes with the keystream from the state's block number on,
// advancing it (see chacha20_encrypt)
static void chacha20_xor_stream(chacha20_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len) {
    if (!ctx || !out) return;
    uint8_t key_stream[64];
    size_t offset = 0;

    while (offset < len) {
        chacha20_block(ctx, key_stream);
        ctx->state[12]++; // Increment counter

        size_t bytes_to_process = (len - offset < 64) ? (len - offset) : 64;
        for (size_t i = 0; i < bytes_to_process; i++) {
            out[offset + i] = (in ? in[offset + i] : 0) ^ key_stream[i];
        }
        offset += bytes_to_process;
    }
}

void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t len) {
    chacha20_ctx_t ctx;
    chacha20_init(&ctx, key, nonce, counter);
    chacha20_xor_stream(&ctx, in, out, len);
}
