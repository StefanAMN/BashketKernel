/**
 * @file md5.c
 * @brief Full RFC 1321 Compliant MD5 Cryptographic Hash Implementation
 */

#include "md5.h"
#include "../string.h"

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(f, a, b, c, d, x, s, ac) { \
    (a) += f((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];

    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        x[i] = (uint32_t)block[j] | ((uint32_t)block[j+1] << 8) |
               ((uint32_t)block[j+2] << 16) | ((uint32_t)block[j+3] << 24);
    }

    /* Round 1 */
    STEP(F, a, b, c, d, x[0], 7, 0xd76aa478);
    STEP(F, d, a, b, c, x[1], 12, 0xe8c7b756);
    STEP(F, c, d, a, b, x[2], 17, 0x242070db);
    STEP(F, b, c, d, a, x[3], 22, 0xc1bdceee);
    STEP(F, a, b, c, d, x[4], 7, 0xf57c0faf);
    STEP(F, d, a, b, c, x[5], 12, 0x4787c62a);
    STEP(F, c, d, a, b, x[6], 17, 0xa8304613);
    STEP(F, b, c, d, a, x[7], 22, 0xfd469501);
    STEP(F, a, b, c, d, x[8], 7, 0x698098d8);
    STEP(F, d, a, b, c, x[9], 12, 0x8b44f7af);
    STEP(F, c, d, a, b, x[10], 17, 0xffff5bb1);
    STEP(F, b, c, d, a, x[11], 22, 0x895cd7be);
    STEP(F, a, b, c, d, x[12], 7, 0x6b901122);
    STEP(F, d, a, b, c, x[13], 12, 0xfd987193);
    STEP(F, c, d, a, b, x[14], 17, 0x86d3d2d4);
    STEP(F, b, c, d, a, x[15], 22, 0x4787c62a);

    /* Round 2 */
    STEP(G, a, b, c, d, x[1], 5, 0xf61e2562);
    STEP(G, d, a, b, c, x[6], 9, 0xc040b340);
    STEP(G, c, d, a, b, x[11], 14, 0x265e5a51);
    STEP(G, b, c, d, a, x[0], 20, 0xe9b6c7aa);
    STEP(G, a, b, c, d, x[5], 5, 0xd62f105d);
    STEP(G, d, a, b, c, x[10], 9, 0x02441453);
    STEP(G, c, d, a, b, x[15], 14, 0xd8a1e681);
    STEP(G, b, c, d, a, x[4], 20, 0xe7d3fbc8);
    STEP(G, a, b, c, d, x[9], 5, 0x21e1cde6);
    STEP(G, d, a, b, c, x[14], 9, 0xc33707d6);
    STEP(G, c, d, a, b, x[3], 14, 0xf4d50d87);
    STEP(G, b, c, d, a, x[8], 20, 0x455a14ed);
    STEP(G, a, b, c, d, x[13], 5, 0xa9e3e905);
    STEP(G, d, a, b, c, x[2], 9, 0xfcefa3f8);
    STEP(G, c, d, a, b, x[7], 14, 0x676f02d9);
    STEP(G, b, c, d, a, x[12], 20, 0x8d2a4c8a);

    /* Round 3 */
    STEP(H, a, b, c, d, x[5], 4, 0xfffa3942);
    STEP(H, d, a, b, c, x[8], 11, 0x8771f681);
    STEP(H, c, d, a, b, x[11], 16, 0x6d9d6122);
    STEP(H, b, c, d, a, x[14], 23, 0xfde5380c);
    STEP(H, a, b, c, d, x[1], 4, 0xa4beea44);
    STEP(H, d, a, b, c, x[4], 11, 0x4bdecfa9);
    STEP(H, c, d, a, b, x[7], 16, 0xf6bb4b60);
    STEP(H, b, c, d, a, x[10], 23, 0xbebfbc70);
    STEP(H, a, b, c, d, x[13], 4, 0x289b7ec6);
    STEP(H, d, a, b, c, x[0], 11, 0xeaa127fa);
    STEP(H, c, d, a, b, x[3], 16, 0xd4ef3085);
    STEP(H, b, c, d, a, x[6], 23, 0x04881d05);
    STEP(H, a, b, c, d, x[9], 4, 0xd9d4d039);
    STEP(H, d, a, b, c, x[12], 11, 0xe6db99e5);
    STEP(H, c, d, a, b, x[15], 16, 0x1fa27cf8);
    STEP(H, b, c, d, a, x[2], 23, 0xc4ac5665);

    /* Round 4 */
    STEP(I, a, b, c, d, x[0], 6, 0xf4292244);
    STEP(I, d, a, b, c, x[7], 10, 0x432aff97);
    STEP(I, c, d, a, b, x[14], 15, 0xab9423a7);
    STEP(I, b, c, d, a, x[5], 21, 0xfc93a039);
    STEP(I, a, b, c, d, x[12], 6, 0x655b59c3);
    STEP(I, d, a, b, c, x[3], 10, 0x8f0ccc92);
    STEP(I, c, d, a, b, x[10], 15, 0xffeff47d);
    STEP(I, b, c, d, a, x[1], 21, 0x85845dd1);
    STEP(I, a, b, c, d, x[8], 6, 0x6fa87e4f);
    STEP(I, d, a, b, c, x[15], 10, 0xfe2ce6e0);
    STEP(I, c, d, a, b, x[6], 15, 0xa3014314);
    STEP(I, b, c, d, a, x[13], 21, 0x4e0811a1);
    STEP(I, a, b, c, d, x[4], 6, 0xf7537e82);
    STEP(I, d, a, b, c, x[11], 10, 0xbd3af235);
    STEP(I, c, d, a, b, x[2], 15, 0x2ad7d2bb);
    STEP(I, b, c, d, a, x[9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void md5_init(md5_ctx_t *ctx) {
    ctx->size = 0;
    ctx->buffer[0] = 0x67452301;
    ctx->buffer[1] = 0xefcdab89;
    ctx->buffer[2] = 0x98badcfe;
    ctx->buffer[3] = 0x10325476;
}

void md5_update(md5_ctx_t *ctx, const uint8_t *input, size_t input_len) {
    size_t index = (size_t)((ctx->size >> 3) & 0x3F);
    ctx->size += (uint64_t)input_len << 3;

    size_t part_len = 64 - index;
    size_t i = 0;

    if (input_len >= part_len) {
        memcpy(&ctx->input[index], input, part_len);
        md5_transform(ctx->buffer, ctx->input);

        for (i = part_len; i + 63 < input_len; i += 64) {
            md5_transform(ctx->buffer, &input[i]);
        }
        index = 0;
    }

    memcpy(&ctx->input[index], &input[i], input_len - i);
}

void md5_final(md5_ctx_t *ctx) {
    uint8_t padding[64] = { 0x80 };
    uint8_t bits[8];

    for (int i = 0; i < 8; i++) {
        bits[i] = (uint8_t)((ctx->size >> (i * 8)) & 0xFF);
    }

    size_t index = (size_t)((ctx->size >> 3) & 0x3F);
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);

    md5_update(ctx, padding, pad_len);
    md5_update(ctx, bits, 8);

    for (int i = 0; i < 4; i++) {
        ctx->digest[i * 4 + 0] = (uint8_t)((ctx->buffer[i] >> 0) & 0xFF);
        ctx->digest[i * 4 + 1] = (uint8_t)((ctx->buffer[i] >> 8) & 0xFF);
        ctx->digest[i * 4 + 2] = (uint8_t)((ctx->buffer[i] >> 16) & 0xFF);
        ctx->digest[i * 4 + 3] = (uint8_t)((ctx->buffer[i] >> 24) & 0xFF);
    }
}

void md5_hash(const uint8_t *data, size_t len, uint8_t hash[16]) {
    md5_ctx_t ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx);
    memcpy(hash, ctx.digest, 16);
}
