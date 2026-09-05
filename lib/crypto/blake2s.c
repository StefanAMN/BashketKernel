/**
 * @file blake2s.c
 * @brief RFC 7693 BLAKE2s-256 Cryptographic Hash Implementation
 */

#include "blake2s.h"
#include "../string.h"

static const uint32_t blake2s_IV[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
};

static const uint8_t blake2s_sigma[10][16] = {
    { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    { 7,   9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    { 9,   0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    { 2,  12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    { 6,  15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 }
};

#define ROTR32(x, y) (((x) >> (y)) ^ ((x) << (32 - (y))))

#define G(r, i, a, b, c, d, m) do { \
    a = a + b + m[blake2s_sigma[r][2*i]]; \
    d = ROTR32(d ^ a, 16); \
    c = c + d; \
    b = ROTR32(b ^ c, 12); \
    a = a + b + m[blake2s_sigma[r][2*i+1]]; \
    d = ROTR32(d ^ a, 8); \
    c = c + d; \
    b = ROTR32(b ^ c, 7); \
} while(0)

static void blake2s_compress(blake2s_ctx_t *ctx, const uint8_t block[64]) {
    uint32_t m[16];
    uint32_t v[16];

    for (size_t i = 0; i < 16; i++) {
        m[i] = ((uint32_t)block[i * 4 + 0]) |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    for (size_t i = 0; i < 8; i++) {
        v[i] = ctx->h[i];
        v[i + 8] = blake2s_IV[i];
    }

    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    v[14] ^= ctx->f[0];
    v[15] ^= ctx->f[1];

    for (size_t r = 0; r < 10; r++) {
        G(r, 0, v[0], v[4], v[8],  v[12], m);
        G(r, 1, v[1], v[5], v[9],  v[13], m);
        G(r, 2, v[2], v[6], v[10], v[14], m);
        G(r, 3, v[3], v[7], v[11], v[15], m);
        G(r, 4, v[0], v[5], v[10], v[15], m);
        G(r, 5, v[1], v[6], v[11], v[12], m);
        G(r, 6, v[2], v[7], v[8],  v[13], m);
        G(r, 7, v[3], v[4], v[9],  v[14], m);
    }

    for (size_t i = 0; i < 8; i++) {
        ctx->h[i] ^= v[i] ^ v[i + 8];
    }
}

void blake2s_init(blake2s_ctx_t *ctx, size_t outlen) {
    if (!ctx || outlen == 0 || outlen > 32) return;
    memset(ctx, 0, sizeof(blake2s_ctx_t));
    ctx->outlen = outlen;
    for (size_t i = 0; i < 8; i++) ctx->h[i] = blake2s_IV[i];
    ctx->h[0] ^= 0x01010000 ^ (uint32_t)outlen;
}

void blake2s_update(blake2s_ctx_t *ctx, const uint8_t *in, size_t inlen) {
    if (!ctx || !in || inlen == 0) return;
    for (size_t i = 0; i < inlen; i++) {
        if (ctx->c == 64) {
            ctx->t[0] += ctx->c;
            if (ctx->t[0] < ctx->c) ctx->t[1]++;
            blake2s_compress(ctx, ctx->b);
            ctx->c = 0;
        }
        ctx->b[ctx->c++] = in[i];
    }
}

void blake2s_final(blake2s_ctx_t *ctx, uint8_t *out) {
    if (!ctx || !out) return;
    ctx->t[0] += ctx->c;
    if (ctx->t[0] < ctx->c) ctx->t[1]++;
    ctx->f[0] = 0xFFFFFFFF;
    memset(ctx->b + ctx->c, 0, 64 - ctx->c);
    blake2s_compress(ctx, ctx->b);

    for (size_t i = 0; i < ctx->outlen; i++) {
        out[i] = (uint8_t)((ctx->h[i >> 2] >> (8 * (i & 3))) & 0xFF);
    }
}

void blake2s_hash(const uint8_t *in, size_t inlen, uint8_t out[32]) {
    blake2s_ctx_t ctx;
    blake2s_init(&ctx, 32);
    blake2s_update(&ctx, in, inlen);
    blake2s_final(&ctx, out);
}
