/**
 * @file poly1305.c
 * @brief RFC 7539 Poly1305 One-Time Authenticator (128-bit MAC) Implementation
 */

#include "poly1305.h"
#include "../string.h"

static uint32_t load32_le(const uint8_t *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static void store32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val & 0xFF);
    dst[1] = (uint8_t)((val >> 8) & 0xFF);
    dst[2] = (uint8_t)((val >> 16) & 0xFF);
    dst[3] = (uint8_t)((val >> 24) & 0xFF);
}

void poly1305_init(poly1305_ctx_t *ctx, const uint8_t key[32]) {
    if (!ctx || !key) return;
    memset(ctx, 0, sizeof(poly1305_ctx_t));

    // Clamp r
    ctx->r[0] = load32_le(key + 0) & 0x0CFFFFFF;
    ctx->r[1] = load32_le(key + 4) & 0x0CA33333;
    ctx->r[2] = load32_le(key + 8) & 0x0CA33333;
    ctx->r[3] = load32_le(key + 12) & 0x0CA33333;
    ctx->r[4] = load32_le(key + 16) & 0x0003FFFF;

    // Store s (pad)
    ctx->pad[0] = load32_le(key + 16);
    ctx->pad[1] = load32_le(key + 20);
    ctx->pad[2] = load32_le(key + 24);
    ctx->pad[3] = load32_le(key + 28);
}

static void poly1305_blocks(poly1305_ctx_t *ctx, const uint8_t *in, size_t len) {
    uint32_t hibit = ctx->final ? 0 : (1 << 24);
    uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    while (len >= 16) {
        uint32_t t0 = load32_le(in + 0);
        uint32_t t1 = load32_le(in + 4);
        uint32_t t2 = load32_le(in + 8);
        uint32_t t3 = load32_le(in + 12);

        h0 += t0 & 0x03FFFFFF;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x03FFFFFF;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x03FFFFFF;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x03FFFFFF;
        h4 += (t3 >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x03FFFFFF; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x03FFFFFF; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x03FFFFFF; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x03FFFFFF; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x03FFFFFF;
        h0 += c * 5;
        c = h0 >> 26; h0 &= 0x03FFFFFF; h1 += c;

        in += 16;
        len -= 16;
    }

    ctx->h[0] = h0; ctx->h[1] = h1; ctx->h[2] = h2; ctx->h[3] = h3; ctx->h[4] = h4;
}

void poly1305_update(poly1305_ctx_t *ctx, const uint8_t *in, size_t len) {
    if (!ctx || !in) return;
    if (ctx->leftover) {
        size_t want = 16 - ctx->leftover;
        if (want > len) want = len;
        memcpy(ctx->buffer + ctx->leftover, in, want);
        ctx->leftover += want;
        in += want;
        len -= want;
        if (ctx->leftover < 16) return;
        poly1305_blocks(ctx, ctx->buffer, 16);
        ctx->leftover = 0;
    }

    if (len >= 16) {
        size_t take = len & ~((size_t)15);
        poly1305_blocks(ctx, in, take);
        in += take;
        len -= take;
    }

    if (len) {
        memcpy(ctx->buffer + ctx->leftover, in, len);
        ctx->leftover += len;
    }
}

void poly1305_final(poly1305_ctx_t *ctx, uint8_t mac[16]) {
    if (!ctx || !mac) return;

    if (ctx->leftover) {
        size_t i = ctx->leftover;
        ctx->buffer[i++] = 1;
        for (; i < 16; i++) ctx->buffer[i] = 0;
        ctx->final = 1;
        poly1305_blocks(ctx, ctx->buffer, 16);
    }

    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];
    uint32_t c = h0 >> 26; h0 &= 0x03FFFFFF; h1 += c;
    c = h1 >> 26; h1 &= 0x03FFFFFF; h2 += c;
    c = h2 >> 26; h2 &= 0x03FFFFFF; h3 += c;
    c = h3 >> 26; h3 &= 0x03FFFFFF; h4 += c;
    c = h4 >> 26; h4 &= 0x03FFFFFF;
    h0 += c * 5;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x03FFFFFF;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x03FFFFFF;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x03FFFFFF;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x03FFFFFF;
    uint32_t g4 = h4 + c - (1 << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    uint64_t f0 = ((uint64_t)h0 | ((uint64_t)h1 << 26)) + ctx->pad[0];
    uint64_t f1 = (((uint64_t)h1 >> 6) | ((uint64_t)h2 << 20)) + ctx->pad[1] + (f0 >> 32);
    uint64_t f2 = (((uint64_t)h2 >> 12) | ((uint64_t)h3 << 14)) + ctx->pad[2] + (f1 >> 32);
    uint64_t f3 = (((uint64_t)h3 >> 18) | ((uint64_t)h4 << 8)) + ctx->pad[3] + (f2 >> 32);

    store32_le(mac + 0, (uint32_t)f0);
    store32_le(mac + 4, (uint32_t)f1);
    store32_le(mac + 8, (uint32_t)f2);
    store32_le(mac + 12, (uint32_t)f3);
}

void poly1305_mac(const uint8_t key[32], const uint8_t *in, size_t len, uint8_t mac[16]) {
    poly1305_ctx_t ctx;
    poly1305_init(&ctx, key);
    poly1305_update(&ctx, in, len);
    poly1305_final(&ctx, mac);
}
