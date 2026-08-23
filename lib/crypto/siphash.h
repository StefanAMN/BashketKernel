/**
 * @file siphash.h
 * @brief SipHash-2-4 Keyed Hash Function Interface (64-bit output, 128-bit key)
 */

#ifndef LIB_CRYPTO_SIPHASH_H
#define LIB_CRYPTO_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

uint64_t siphash24(const uint8_t key[16], const uint8_t *data, size_t len);

#endif // LIB_CRYPTO_SIPHASH_H
