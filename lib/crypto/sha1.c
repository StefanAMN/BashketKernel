/**
 * @file sha1.c
 * @brief FIPS PUB 180-1 Standard SHA-1 Cryptographic Hash Implementation
 */

#include "sha1.h"
#include "../string.h"

#define ROTLEFT(a,b) (((a) << (b)) | ((a) >> (32 - (b))))

static void sha1_transform(sha1_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, i, j, t, m[80];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (; i < 80; ++i)
        m[i] = ROTLEFT(m[i - 3] ^ m[i - 8] ^ m[i - 14] ^ m[i - 16], 1);

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];

    for (i = 0; i < 80; ++i) {
        if (i < 20) {
            t = ROTLEFT(a, 5) + ((b & c) ^ (~b & d)) + e + 0x5a827999 + m[i];
        } else if (i < 40) {
            t = ROTLEFT(a, 5) + (b ^ c ^ d) + e + 0x6ed9eba1 + m[i];
        } else if (i < 60) {
            t = ROTLEFT(a, 5) + ((b & c) ^ (b & d) ^ (c & d)) + e + 0x8f1bbcdc + m[i];
        } else {
            t = ROTLEFT(a, 5) + (b ^ c ^ d) + e + 0xca62c1d6 + m[i];
        }
        e = d; d = c; c = ROTLEFT(b, 30); b = a; a = t;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

void sha1_init(sha1_ctx_t *ctx) {
    if (!ctx) return;
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xc3d2e1f0;
}

void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx || !data) return;
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha1_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void sha1_final(sha1_ctx_t *ctx, uint8_t hash[20]) {
    if (!ctx || !hash) return;
    size_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha1_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += ctx->datalen * 8;
    ctx->data[56] = (ctx->bitlen >> 56) & 0xFF;
    ctx->data[57] = (ctx->bitlen >> 48) & 0xFF;
    ctx->data[58] = (ctx->bitlen >> 40) & 0xFF;
    ctx->data[59] = (ctx->bitlen >> 32) & 0xFF;
    ctx->data[60] = (ctx->bitlen >> 24) & 0xFF;
    ctx->data[61] = (ctx->bitlen >> 16) & 0xFF;
    ctx->data[62] = (ctx->bitlen >> 8) & 0xFF;
    ctx->data[63] = ctx->bitlen & 0xFF;
    sha1_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xFF;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xFF;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xFF;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xFF;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xFF;
    }
}

void sha1_hash(const uint8_t *data, size_t len, uint8_t hash[20]) {
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, hash);
}
