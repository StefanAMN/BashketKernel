/**
 * @file chacha20.h
 * @brief RFC 7539 ChaCha20 256-bit Stream Cipher Interface
 */

#ifndef LIB_CRYPTO_CHACHA20_H
#define LIB_CRYPTO_CHACHA20_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[16];
} chacha20_ctx_t;

void chacha20_init(chacha20_ctx_t *ctx, const uint8_t key[32], const uint8_t nonce[12], uint32_t counter);
void chacha20_xor_stream(chacha20_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len);
void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t len);

#endif // LIB_CRYPTO_CHACHA20_H
