/**
 * @file hmac.h
 * @brief RFC 2104 Keyed-Hashing for Message Authentication Code Interface
 */

#ifndef LIB_CRYPTO_HMAC_H
#define LIB_CRYPTO_HMAC_H

#include <stddef.h>
#include <stdint.h>

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[32]);
void hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[64]);
void hmac_md5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[16]);

#endif // LIB_CRYPTO_HMAC_H
