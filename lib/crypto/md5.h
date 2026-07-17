/**
 * @file md5.h
 * @brief MD5 Hash Algorithm Interface (RFC 1321 Standard)
 */

#ifndef LIB_CRYPTO_MD5_H
#define LIB_CRYPTO_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t size;
    uint32_t buffer[4];
    uint8_t input[64];
    uint8_t digest[16];
} md5_ctx_t;

void md5_init(md5_ctx_t *ctx);
void md5_update(md5_ctx_t *ctx, const uint8_t *input, size_t input_len);
void md5_final(md5_ctx_t *ctx);
void md5_hash(const uint8_t *data, size_t len, uint8_t hash[16]);

#endif // LIB_CRYPTO_MD5_H
