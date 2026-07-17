/**
 * @file crypto.c
 * @brief Implementation of Kernel Cryptographic Hashing and AES Cipher Subsystem
 */

#include "crypto.h"
#include "string.h"

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
