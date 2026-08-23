/**
 * @file hkdf.h
 * @brief RFC 5869 HMAC-based Extract-and-Expand Key Derivation Function (HKDF) Interface
 */

#ifndef LIB_CRYPTO_HKDF_H
#define LIB_CRYPTO_HKDF_H

#include <stddef.h>
#include <stdint.h>

void hkdf_extract_sha256(const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         uint8_t prk[32]);

void hkdf_expand_sha256(const uint8_t prk[32],
                        const uint8_t *info, size_t info_len,
                        uint8_t *okm, size_t okm_len);

void hkdf_sha256(const uint8_t *salt, size_t salt_len,
                 const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *okm, size_t okm_len);

#endif // LIB_CRYPTO_HKDF_H
