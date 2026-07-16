/**
 * @file crypto.h
 * @brief Cryptographic Primitives (SHA-256 & AES-128) Interface
 */

#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Calculate 256-bit SHA-256 digest hash of input buffer.
 * @param data Input byte buffer.
 * @param len Buffer length.
 * @param hash Output 32-byte hash array.
 */
void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[32]);

/**
 * @brief Encrypt 16-byte block using AES-128 key.
 * @param key 16-byte secret key.
 * @param block 16-byte data block to encrypt in-place.
 */
void aes128_encrypt_block(const uint8_t key[16], uint8_t block[16]);

#endif // CRYPTO_H
