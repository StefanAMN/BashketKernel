/**
 * @file aes_modes.h
 * @brief AES-XTS (IEEE 1619) and AES-GCM (NIST SP 800-38D) Interface
 */

#ifndef LIB_CRYPTO_AES_MODES_H
#define LIB_CRYPTO_AES_MODES_H

#include <stddef.h>
#include <stdint.h>

// AES-XTS Disk Encryption Mode
void aes128_xts_encrypt(const uint8_t key[32], uint64_t sector, const uint8_t *in, uint8_t *out, size_t len);
void aes128_xts_decrypt(const uint8_t key[32], uint64_t sector, const uint8_t *in, uint8_t *out, size_t len);

// AES-GCM Authenticated Encryption with Associated Data (AEAD)
void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                         const uint8_t *aad, size_t aad_len,
                         const uint8_t *in, uint8_t *out, size_t len,
                         uint8_t tag[16]);

int aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, uint8_t *out, size_t len,
                        const uint8_t tag[16]);

#endif // LIB_CRYPTO_AES_MODES_H
