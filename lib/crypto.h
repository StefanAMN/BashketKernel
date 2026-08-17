/**
 * @file crypto.h
 * @brief Kernel Cryptographic Primitives (Hashing & Cipher Encryption/Decryption)
 */

#ifndef LIB_CRYPTO_H
#define LIB_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* --- Hashing Algorithms --- */

/**
 * @brief Calculate 256-bit SHA-256 digest hash of input buffer.
 * @param data Input byte buffer.
 * @param len Buffer length.
 * @param hash Output 32-byte hash array.
 */
void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[32]);

/**
 * @brief Calculate 128-bit MD5 digest hash of input buffer.
 * @param data Input byte buffer.
 * @param len Buffer length.
 * @param hash Output 16-byte hash array.
 */
void md5_hash(const uint8_t *data, size_t len, uint8_t hash[16]);

/* --- Symmetric Cipher Encryption & Decryption --- */

/**
 * @brief Encrypt 16-byte block using AES-128 key.
 * @param key 16-byte secret key.
 * @param block 16-byte data block to encrypt in-place.
 */
void aes128_encrypt_block(const uint8_t key[16], uint8_t block[16]);

/**
 * @brief Decrypt 16-byte block using AES-128 key.
 * @param key 16-byte secret key.
 * @param block 16-byte encrypted block to decrypt in-place.
 */
void aes128_decrypt_block(const uint8_t key[16], uint8_t block[16]);

#endif // LIB_CRYPTO_H
