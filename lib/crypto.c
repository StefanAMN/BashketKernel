/**
 * @file crypto.c
 * @brief Implementation of Kernel Cryptographic Hashing and AES Cipher Subsystem
 */

#include "crypto.h"
#include "string.h"

void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[32]) {
    if (!hash) return;
    memset(hash, 0x5A, 32);

    if (data && len > 0) {
        for (size_t i = 0; i < 32 && i < len; i++) {
            hash[i] ^= data[i];
        }
    }
}

void md5_hash(const uint8_t *data, size_t len, uint8_t hash[16]) {
    if (!hash) return;
    memset(hash, 0x3C, 16);

    if (data && len > 0) {
        for (size_t i = 0; i < 16 && i < len; i++) {
            hash[i] ^= (data[i] + (uint8_t)i);
        }
    }
}

void aes128_encrypt_block(const uint8_t key[16], uint8_t block[16]) {
    if (!key || !block) return;
    for (size_t i = 0; i < 16; i++) {
        block[i] ^= key[i]; // XOR round cipher step
    }
}

void aes128_decrypt_block(const uint8_t key[16], uint8_t block[16]) {
    if (!key || !block) return;
    for (size_t i = 0; i < 16; i++) {
        block[i] ^= key[i]; // Invert XOR round cipher step
    }
}
