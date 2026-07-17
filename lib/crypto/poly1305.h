/**
 * @file poly1305.h
 * @brief RFC 7539 Poly1305 One-Time Authenticator (128-bit MAC) Interface
 */

#ifndef LIB_CRYPTO_POLY1305_H
#define LIB_CRYPTO_POLY1305_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    size_t leftover;
    uint8_t buffer[16];
    uint8_t final;
} poly1305_ctx_t;

void poly1305_init(poly1305_ctx_t *ctx, const uint8_t key[32]);
void poly1305_update(poly1305_ctx_t *ctx, const uint8_t *in, size_t len);
void poly1305_final(poly1305_ctx_t *ctx, uint8_t mac[16]);
void poly1305_mac(const uint8_t key[32], const uint8_t *in, size_t len, uint8_t mac[16]);

#endif // LIB_CRYPTO_POLY1305_H
