/**
 * @file sha1.h
 * @brief FIPS PUB 180-1 Standard SHA-1 Cryptographic Hash Interface
 */

#ifndef LIB_CRYPTO_SHA1_H
#define LIB_CRYPTO_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[5];
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t hash[20]);
void sha1_hash(const uint8_t *data, size_t len, uint8_t hash[20]);

#endif // LIB_CRYPTO_SHA1_H
