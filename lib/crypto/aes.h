/**
 * @file aes.h
 * @brief AES-128 Symmetric Block Cipher Interface (FIPS PUB 197 Standard)
 */

#ifndef LIB_CRYPTO_AES_H
#define LIB_CRYPTO_AES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t round_keys[176];
} aes128_ctx_t;

void aes128_init(aes128_ctx_t *ctx, const uint8_t key[16]);
void aes128_encrypt(const aes128_ctx_t *ctx, uint8_t block[16]);
void aes128_decrypt(const aes128_ctx_t *ctx, uint8_t block[16]);

// Convenience 1-shot API
void aes128_encrypt_block(const uint8_t key[16], uint8_t block[16]);
void aes128_decrypt_block(const uint8_t key[16], uint8_t block[16]);

#endif // LIB_CRYPTO_AES_H
