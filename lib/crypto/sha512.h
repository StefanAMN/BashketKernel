/**
 * @file sha512.h
 * @brief FIPS PUB 180-4 Standard SHA-512 Cryptographic Hash Interface
 */

#ifndef LIB_CRYPTO_SHA512_H
#define LIB_CRYPTO_SHA512_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t data[128];
    uint32_t datalen;
    uint64_t bitlen[2];
    uint64_t state[8];
} sha512_ctx_t;

void sha512_init(sha512_ctx_t *ctx);
void sha512_update(sha512_ctx_t *ctx, const uint8_t *data, size_t len);
void sha512_final(sha512_ctx_t *ctx, uint8_t hash[64]);
void sha512_hash(const uint8_t *data, size_t len, uint8_t hash[64]);

#endif // LIB_CRYPTO_SHA512_H
