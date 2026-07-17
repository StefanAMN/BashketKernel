/**
 * @file hmac.c
 * @brief RFC 2104 Keyed-Hashing for Message Authentication Code Implementation
 */

#include "hmac.h"
#include "sha256.h"
#include "sha512.h"
#include "md5.h"
#include "../string.h"

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[32]) {
    uint8_t k_pad[64];
    uint8_t k_hash[32];
    memset(k_pad, 0, 64);

    if (key_len > 64) {
        sha256_hash(key, key_len, k_hash);
        memcpy(k_pad, k_hash, 32);
    } else {
        memcpy(k_pad, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5C;
    }

    // Inner hash: H(ipad || data)
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, data, data_len);
    uint8_t inner_hash[32];
    sha256_final(&ctx, inner_hash);

    // Outer hash: H(opad || inner_hash)
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner_hash, 32);
    sha256_final(&ctx, mac);
}

void hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[64]) {
    uint8_t k_pad[128];
    uint8_t k_hash[64];
    memset(k_pad, 0, 128);

    if (key_len > 128) {
        sha512_hash(key, key_len, k_hash);
        memcpy(k_pad, k_hash, 64);
    } else {
        memcpy(k_pad, key, key_len);
    }

    uint8_t ipad[128];
    uint8_t opad[128];
    for (int i = 0; i < 128; i++) {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5C;
    }

    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, ipad, 128);
    sha512_update(&ctx, data, data_len);
    uint8_t inner_hash[64];
    sha512_final(&ctx, inner_hash);

    sha512_init(&ctx);
    sha512_update(&ctx, opad, 128);
    sha512_update(&ctx, inner_hash, 64);
    sha512_final(&ctx, mac);
}

void hmac_md5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[16]) {
    uint8_t k_pad[64];
    uint8_t k_hash[16];
    memset(k_pad, 0, 64);

    if (key_len > 64) {
        md5_hash(key, key_len, k_hash);
        memcpy(k_pad, k_hash, 16);
    } else {
        memcpy(k_pad, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5C;
    }

    md5_ctx_t ctx;
    md5_init(&ctx);
    md5_update(&ctx, ipad, 64);
    md5_update(&ctx, data, data_len);
    md5_final(&ctx);
    uint8_t inner_hash[16];
    memcpy(inner_hash, ctx.digest, 16);

    md5_init(&ctx);
    md5_update(&ctx, opad, 64);
    md5_update(&ctx, inner_hash, 16);
    md5_final(&ctx);
    memcpy(mac, ctx.digest, 16);
}
