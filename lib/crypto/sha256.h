/**
 * @file sha256.h
 * @brief The SHA-256 hash (FIPS 180-4), for password hashing (/bin/login's
 *        PBKDF2) and the random generator
 *
 * This file is compiled twice: into the kernel and into the user library.
 */

#ifndef LIB_CRYPTO_SHA256_H
#define LIB_CRYPTO_SHA256_H

#include <stddef.h>
#include <stdint.h>

// A hash being computed: the block being filled, how many bytes it holds,
// the bits hashed in earlier blocks, and the state words
typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

/**
 * @brief Start a new hash.
 */
void sha256_init(sha256_ctx_t *ctx);

/**
 * @brief Add `len` bytes to the hash (any number of calls, any sizes).
 */
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Finish: pad, and write the 32-byte hash. The context is used up
 *        (sha256_init again to reuse it).
 */
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]);

/**
 * @brief The hash of one block of `len` bytes, in one call.
 */
void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[32]);

#endif // LIB_CRYPTO_SHA256_H
