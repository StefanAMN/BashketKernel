/**
 * @file aes.h
 * @brief FIPS 197 Advanced Encryption Standard (AES-128 and AES-256) Interface
 */

#ifndef LIB_CRYPTO_AES_H
#define LIB_CRYPTO_AES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t round_keys[60]; // Up to 15 round keys (4 * 15 = 60 words for AES-256)
    int rounds;              // 10 for AES-128, 14 for AES-256
} aes_ctx_t;

// AES-128 API
void aes128_init(aes_ctx_t *ctx, const uint8_t key[16]);
void aes128_encrypt(const aes_ctx_t *ctx, uint8_t block[16]);
void aes128_decrypt(const aes_ctx_t *ctx, uint8_t block[16]);

// AES-256 API
void aes256_init(aes_ctx_t *ctx, const uint8_t key[32]);
void aes256_encrypt(const aes_ctx_t *ctx, uint8_t block[16]);
void aes256_decrypt(const aes_ctx_t *ctx, uint8_t block[16]);

// Convenience 1-shot API
void aes128_encrypt_block(const uint8_t key[16], uint8_t block[16]);
void aes128_decrypt_block(const uint8_t key[16], uint8_t block[16]);
void aes256_encrypt_block(const uint8_t key[32], uint8_t block[16]);
void aes256_decrypt_block(const uint8_t key[32], uint8_t block[16]);

#endif // LIB_CRYPTO_AES_H
