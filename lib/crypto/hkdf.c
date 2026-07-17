/**
 * @file hkdf.c
 * @brief RFC 5869 HMAC-based Extract-and-Expand Key Derivation Function (HKDF-SHA256) Implementation
 */

#include "hkdf.h"
#include "hmac.h"
#include "../string.h"

void hkdf_extract_sha256(const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         uint8_t prk[32]) {
    uint8_t null_salt[32] = {0};
    if (!salt || salt_len == 0) {
        salt = null_salt;
        salt_len = 32;
    }

    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

void hkdf_expand_sha256(const uint8_t prk[32],
                        const uint8_t *info, size_t info_len,
                        uint8_t *okm, size_t okm_len) {
    if (!prk || !okm || okm_len == 0) return;

    size_t n = (okm_len + 31) / 32;
    uint8_t t[32];
    size_t t_len = 0;
    size_t offset = 0;

    for (size_t i = 1; i <= n; i++) {
        uint8_t counter = (uint8_t)i;
        size_t block_data_len = t_len + info_len + 1;
        uint8_t block_data[256];

        if (t_len > 0) {
            memcpy(block_data, t, t_len);
        }
        if (info && info_len > 0) {
            memcpy(block_data + t_len, info, info_len);
        }
        block_data[t_len + info_len] = counter;

        hmac_sha256(prk, 32, block_data, block_data_len, t);
        t_len = 32;

        size_t copy_bytes = (okm_len - offset < 32) ? (okm_len - offset) : 32;
        memcpy(okm + offset, t, copy_bytes);
        offset += copy_bytes;
    }
}

void hkdf_sha256(const uint8_t *salt, size_t salt_len,
                 const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *okm, size_t okm_len) {
    uint8_t prk[32];
    hkdf_extract_sha256(salt, salt_len, ikm, ikm_len, prk);
    hkdf_expand_sha256(prk, info, info_len, okm, okm_len);
}
