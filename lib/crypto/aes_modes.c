/**
 * @file aes_modes.c
 * @brief AES-XTS (IEEE 1619) and AES-GCM (NIST SP 800-38D) Implementation
 */

#include "aes_modes.h"
#include "aes.h"
#include "../string.h"

/* --- GF(2^128) Multiplication Helper for XTS and GCM --- */

// XTS polynomial GF(2^128): x^128 + x^7 + x^2 + x + 1 (0x87)
static void xts_mul_x(uint8_t T[16]) {
    uint8_t carry = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t next_carry = (T[i] >> 7) & 1;
        T[i] = (T[i] << 1) | carry;
        carry = next_carry;
    }
    if (carry) {
        T[0] ^= 0x87;
    }
}

void aes128_xts_encrypt(const uint8_t key[32], uint64_t sector, const uint8_t *in, uint8_t *out, size_t len) {
    if (!key || !in || !out || len < 16) return;

    aes_ctx_t key1_ctx, key2_ctx;
    aes128_init(&key1_ctx, key);
    aes128_init(&key2_ctx, key + 16);

    // Initial tweak block
    uint8_t T[16] = {0};
    T[0] = (uint8_t)(sector & 0xFF);
    T[1] = (uint8_t)((sector >> 8) & 0xFF);
    T[2] = (uint8_t)((sector >> 16) & 0xFF);
    T[3] = (uint8_t)((sector >> 24) & 0xFF);
    T[4] = (uint8_t)((sector >> 32) & 0xFF);
    T[5] = (uint8_t)((sector >> 40) & 0xFF);
    T[6] = (uint8_t)((sector >> 48) & 0xFF);
    T[7] = (uint8_t)((sector >> 56) & 0xFF);

    aes128_encrypt(&key2_ctx, T);

    size_t blocks = len / 16;
    for (size_t b = 0; b < blocks; b++) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) {
            block[i] = in[b * 16 + i] ^ T[i];
        }

        aes128_encrypt(&key1_ctx, block);

        for (int i = 0; i < 16; i++) {
            out[b * 16 + i] = block[i] ^ T[i];
        }

        xts_mul_x(T);
    }
}

void aes128_xts_decrypt(const uint8_t key[32], uint64_t sector, const uint8_t *in, uint8_t *out, size_t len) {
    if (!key || !in || !out || len < 16) return;

    aes_ctx_t key1_ctx, key2_ctx;
    aes128_init(&key1_ctx, key);
    aes128_init(&key2_ctx, key + 16);

    uint8_t T[16] = {0};
    T[0] = (uint8_t)(sector & 0xFF);
    T[1] = (uint8_t)((sector >> 8) & 0xFF);
    T[2] = (uint8_t)((sector >> 16) & 0xFF);
    T[3] = (uint8_t)((sector >> 24) & 0xFF);
    T[4] = (uint8_t)((sector >> 32) & 0xFF);
    T[5] = (uint8_t)((sector >> 40) & 0xFF);
    T[6] = (uint8_t)((sector >> 48) & 0xFF);
    T[7] = (uint8_t)((sector >> 56) & 0xFF);

    aes128_encrypt(&key2_ctx, T);

    size_t blocks = len / 16;
    for (size_t b = 0; b < blocks; b++) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) {
            block[i] = in[b * 16 + i] ^ T[i];
        }

        aes128_decrypt(&key1_ctx, block);

        for (int i = 0; i < 16; i++) {
            out[b * 16 + i] = block[i] ^ T[i];
        }

        xts_mul_x(T);
    }
}

/* --- AES-GCM GHASH Implementation --- */

static void gcm_gf_mul(uint8_t x[16], const uint8_t y[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, y, 16);

    for (int i = 0; i < 128; i++) {
        if ((x[i / 8] >> (7 - (i % 8))) & 1) {
            for (int j = 0; j < 16; j++) z[j] ^= v[j];
        }
        uint8_t lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) {
            v[j] = (v[j] >> 1) | (v[j - 1] << 7);
        }
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1; // GCM reduction polynomial
    }
    memcpy(x, z, 16);
}

static void ghash(const uint8_t H[16], const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t out[16]) {
    memset(out, 0, 16);
    uint8_t block[16];

    // Process AAD
    size_t offset = 0;
    while (offset < aad_len) {
        memset(block, 0, 16);
        size_t chunk = (aad_len - offset < 16) ? (aad_len - offset) : 16;
        memcpy(block, aad + offset, chunk);
        for (int i = 0; i < 16; i++) out[i] ^= block[i];
        gcm_gf_mul(out, H);
        offset += chunk;
    }

    // Process Ciphertext
    offset = 0;
    while (offset < ct_len) {
        memset(block, 0, 16);
        size_t chunk = (ct_len - offset < 16) ? (ct_len - offset) : 16;
        memcpy(block, ct + offset, chunk);
        for (int i = 0; i < 16; i++) out[i] ^= block[i];
        gcm_gf_mul(out, H);
        offset += chunk;
    }

    // Process bit lengths
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t ct_bits = (uint64_t)ct_len * 8;
    memset(block, 0, 16);
    for (int i = 0; i < 8; i++) {
        block[i] = (uint8_t)((aad_bits >> (56 - i * 8)) & 0xFF);
        block[i + 8] = (uint8_t)((ct_bits >> (56 - i * 8)) & 0xFF);
    }
    for (int i = 0; i < 16; i++) out[i] ^= block[i];
    gcm_gf_mul(out, H);
}

void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                         const uint8_t *aad, size_t aad_len,
                         const uint8_t *in, uint8_t *out, size_t len,
                         uint8_t tag[16]) {
    aes_ctx_t ctx;
    aes128_init(&ctx, key);

    uint8_t H[16] = {0};
    aes128_encrypt(&ctx, H);

    uint8_t J0[16] = {0};
    memcpy(J0, iv, 12);
    J0[15] = 1;

    // CTR mode encryption
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);

    for (size_t offset = 0; offset < len; offset += 16) {
        // Increment CTR counter
        for (int i = 15; i >= 12; i--) {
            if (++ctr[i] != 0) break;
        }
        uint8_t ks[16];
        memcpy(ks, ctr, 16);
        aes128_encrypt(&ctx, ks);

        size_t chunk = (len - offset < 16) ? (len - offset) : 16;
        for (size_t i = 0; i < chunk; i++) {
            out[offset + i] = (in ? in[offset + i] : 0) ^ ks[i];
        }
    }

    // Tag generation
    uint8_t S[16];
    ghash(H, aad, aad_len, out, len, S);

    uint8_t J0_enc[16];
    memcpy(J0_enc, J0, 16);
    aes128_encrypt(&ctx, J0_enc);

    for (int i = 0; i < 16; i++) {
        tag[i] = S[i] ^ J0_enc[i];
    }
}

int aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, uint8_t *out, size_t len,
                        const uint8_t tag[16]) {
    aes_ctx_t ctx;
    aes128_init(&ctx, key);

    uint8_t H[16] = {0};
    aes128_encrypt(&ctx, H);

    uint8_t J0[16] = {0};
    memcpy(J0, iv, 12);
    J0[15] = 1;

    // Verify tag
    uint8_t S[16];
    ghash(H, aad, aad_len, in, len, S);

    uint8_t J0_enc[16];
    memcpy(J0_enc, J0, 16);
    aes128_encrypt(&ctx, J0_enc);

    uint8_t calc_tag[16];
    for (int i = 0; i < 16; i++) {
        calc_tag[i] = S[i] ^ J0_enc[i];
    }

    if (memcmp(calc_tag, tag, 16) != 0) {
        return -1; // Authentication failure
    }

    // Decrypt CTR
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);

    for (size_t offset = 0; offset < len; offset += 16) {
        for (int i = 15; i >= 12; i--) {
            if (++ctr[i] != 0) break;
        }
        uint8_t ks[16];
        memcpy(ks, ctr, 16);
        aes128_encrypt(&ctx, ks);

        size_t chunk = (len - offset < 16) ? (len - offset) : 16;
        for (size_t i = 0; i < chunk; i++) {
            out[offset + i] = in[offset + i] ^ ks[i];
        }
    }

    return 0; // Success
}
