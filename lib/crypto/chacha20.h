/**
 * @file chacha20.h
 * @brief The ChaCha20 stream cipher (RFC 8439), for the random generator
 *
 * Checked against the RFC's test vectors (tests/test_dev.c), which agree
 * with an independent implementation.
 */

#ifndef LIB_CRYPTO_CHACHA20_H
#define LIB_CRYPTO_CHACHA20_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief XOR `len` bytes of `in` with the keystream for `key` and `nonce`,
 *        starting at 64-byte block number `counter`, into `out` (which may
 *        be `in`). With `in` NULL, `out` receives the keystream itself.
 *        Encryption and decryption are the same operation.
 */
void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t len);

#endif // LIB_CRYPTO_CHACHA20_H
