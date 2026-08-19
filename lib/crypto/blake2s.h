/**
 * @file blake2s.h
 * @brief RFC 7693 BLAKE2s-256 Cryptographic Hash Interface
 */

#ifndef LIB_CRYPTO_BLAKE2S_H
#define LIB_CRYPTO_BLAKE2S_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t b[64];
    size_t c;
    uint32_t h[8];
    uint32_t t[2];
    uint32_t f[2];
    size_t outlen;
} blake2s_ctx_t;

void blake2s_init(blake2s_ctx_t *ctx, size_t outlen);
void blake2s_update(blake2s_ctx_t *ctx, const uint8_t *in, size_t inlen);
void blake2s_final(blake2s_ctx_t *ctx, uint8_t *out);
void blake2s_hash(const uint8_t *in, size_t inlen, uint8_t out[32]);

#endif // LIB_CRYPTO_BLAKE2S_H
