/**
 * @file ed25519.h
 * @brief RFC 8032 Ed25519 Edwards-curve Digital Signature Algorithm Interface
 */

#ifndef LIB_CRYPTO_ED25519_H
#define LIB_CRYPTO_ED25519_H

#include <stddef.h>
#include <stdint.h>

void ed25519_create_keypair(uint8_t public_key[32], uint8_t private_key[64], const uint8_t seed[32]);
void ed25519_sign(uint8_t signature[64], const uint8_t *message, size_t message_len,
                  const uint8_t public_key[32], const uint8_t private_key[64]);
int ed25519_verify(const uint8_t signature[64], const uint8_t *message, size_t message_len,
                   const uint8_t public_key[32]);

#endif // LIB_CRYPTO_ED25519_H
