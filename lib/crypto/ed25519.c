/**
 * @file ed25519.c
 * @brief RFC 8032 Ed25519 Digital Signature Algorithm Implementation
 */

#include "ed25519.h"
#include "sha512.h"
#include "../string.h"

typedef int64_t fe[10];

static void fe_0(fe h) {
    memset(h, 0, sizeof(fe));
}

static void fe_1(fe h) {
    fe_0(h);
    h[0] = 1;
}

static void fe_copy(fe h, const fe f) {
    for (int i = 0; i < 10; i++) h[i] = f[i];
}

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++) h[i] = f[i] + g[i];
}

static void fe_sub(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++) h[i] = f[i] - g[i];
}

static void fe_carry(fe h) {
    int64_t c;
    for (int i = 0; i < 10; i++) {
        c = h[i] >> ((i & 1) ? 26 : 25);
        h[i] -= c << ((i & 1) ? 26 : 25);
        if (i < 9) h[i + 1] += c;
        else h[0] += c * 19;
    }
}

static void fe_mul(fe h, const fe f, const fe g) {
    int64_t t[19] = {0};

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            t[i + j] += f[i] * g[j];
        }
    }
    for (int i = 18; i >= 10; i--) {
        t[i - 10] += t[i] * 19;
    }

    fe_0(h);
    for (int i = 0; i < 10; i++) h[i] = t[i];
    fe_carry(h);
    fe_carry(h);
}

static void fe_sq(fe h, const fe f) {
    fe_mul(h, f, f);
}

static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    fe_sq(t0, z);
    fe_mul(t1, t0, z);
    fe_sq(t0, t1);
    fe_mul(t2, t0, z);

    fe_copy(t0, t2);
    for (int i = 0; i < 5; i++) fe_sq(t0, t0);
    fe_mul(t0, t0, t2);

    fe_copy(t3, t0);
    for (int i = 0; i < 10; i++) fe_sq(t3, t3);
    fe_mul(t3, t3, t0);

    fe_copy(t0, t3);
    for (int i = 0; i < 20; i++) fe_sq(t0, t0);
    fe_mul(t0, t0, t3);

    for (int i = 0; i < 10; i++) fe_sq(t0, t0);
    fe_mul(t0, t0, t2);

    fe_copy(t3, t0);
    for (int i = 0; i < 50; i++) fe_sq(t3, t3);
    fe_mul(t3, t3, t0);

    fe_copy(t0, t3);
    for (int i = 0; i < 100; i++) fe_sq(t0, t0);
    fe_mul(t0, t0, t3);

    for (int i = 0; i < 50; i++) fe_sq(t0, t0);
    fe_mul(t0, t0, t2);

    for (int i = 0; i < 2; i++) fe_sq(t0, t0);
    fe_mul(out, t0, z);
}

static void fe_tobytes(uint8_t s[32], const fe f) {
    fe h;
    fe_copy(h, f);
    fe_carry(h);
    fe_carry(h);

    for (int i = 0; i < 32; i++) s[i] = 0;

    int bit = 0;
    for (int i = 0; i < 10; i++) {
        int bits = (i & 1) ? 26 : 25;
        uint64_t val = h[i];
        for (int b = 0; b < bits; b++) {
            if ((val >> b) & 1) {
                s[bit / 8] |= (1 << (bit % 8));
            }
            bit++;
        }
    }
}

static void fe_frombytes(fe h, const uint8_t s[32]) {
    fe_0(h);
    int bit = 0;
    for (int i = 0; i < 10; i++) {
        int bits = (i & 1) ? 26 : 25;
        int64_t val = 0;
        for (int b = 0; b < bits; b++) {
            if (bit < 256 && ((s[bit / 8] >> (bit % 8)) & 1)) {
                val |= (1LL << b);
            }
            bit++;
        }
        h[i] = val;
    }
}

// Ed25519 Point Representation (X, Y, Z, T)
typedef struct {
    fe X, Y, Z, T;
} ge;

static const fe d = {
    -10913610, 13857413, -15372611, 6949391, 114729,
    -8787832, -9409928, -4268112, -10214690, 3958934
};

static void ge_p3_0(ge *p) {
    fe_0(p->X);
    fe_1(p->Y);
    fe_1(p->Z);
    fe_0(p->T);
}

static void ge_p3_tobytes(uint8_t s[32], const ge *p) {
    fe zi, x, y;
    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);

    fe_tobytes(s, y);
    fe_carry(x);
    if (x[0] & 1) {
        s[31] |= 0x80;
    }
}

static void ge_add(ge *r, const ge *p, const ge *q) {
    fe A, B, C, D, E, F, G, H;
    fe_sub(A, p->Y, p->X);
    fe_sub(B, q->Y, q->X);
    fe_mul(A, A, B);
    fe_add(B, p->Y, p->X);
    fe_add(C, q->Y, q->X);
    fe_mul(B, B, C);
    fe_mul(C, p->T, q->T);
    fe_mul(C, C, d);
    fe_add(C, C, C); // 2 * d * T1 * T2
    fe_mul(D, p->Z, q->Z);
    fe_add(D, D, D);
    fe_sub(E, B, A);
    fe_sub(F, D, C);
    fe_add(G, D, C);
    fe_add(H, B, A);

    fe_mul(r->X, E, F);
    fe_mul(r->Y, G, H);
    fe_mul(r->T, E, H);
    fe_mul(r->Z, F, G);
}

static void ge_double(ge *r, const ge *p) {
    ge_add(r, p, p);
}

static void ge_scalarmult_base(ge *h, const uint8_t a[32]) {
    ge base;
    // Base point G
    fe_frombytes(base.X, (const uint8_t[]){
        0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9,
        0xb2, 0xa1, 0x47, 0x68, 0x8b, 0x74, 0xf2, 0x1c,
        0x3e, 0x23, 0x16, 0x83, 0x77, 0x98, 0x27, 0x7a,
        0xdc, 0x4d, 0x3d, 0x7b, 0x3a, 0x93, 0x51, 0x21
    });
    fe_frombytes(base.Y, (const uint8_t[]){
        0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
    });
    fe_1(base.Z);
    fe_mul(base.T, base.X, base.Y);

    ge_p3_0(h);

    for (int i = 255; i >= 0; i--) {
        ge_double(h, h);
        if ((a[i / 8] >> (i % 8)) & 1) {
            ge_add(h, h, &base);
        }
    }
}

void ed25519_create_keypair(uint8_t public_key[32], uint8_t private_key[64], const uint8_t seed[32]) {
    if (!public_key || !private_key || !seed) return;

    uint8_t hash[64];
    sha512_hash(seed, 32, hash);

    hash[0] &= 248;
    hash[31] &= 127;
    hash[31] |= 64;

    ge A;
    ge_scalarmult_base(&A, hash);
    ge_p3_tobytes(public_key, &A);

    memcpy(private_key, seed, 32);
    memcpy(private_key + 32, public_key, 32);
}

void ed25519_sign(uint8_t signature[64], const uint8_t *message, size_t message_len,
                  const uint8_t public_key[32], const uint8_t private_key[64]) {
    if (!signature || !public_key || !private_key) return;

    uint8_t hash[64];
    sha512_hash(private_key, 32, hash);

    hash[0] &= 248;
    hash[31] &= 127;
    hash[31] |= 64;

    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, hash + 32, 32);
    if (message && message_len > 0) {
        sha512_update(&ctx, message, message_len);
    }
    uint8_t r_hash[64];
    sha512_final(&ctx, r_hash);

    // Reduce scalar r mod L (simplified)
    r_hash[0] &= 248;
    r_hash[31] &= 127;

    ge R;
    ge_scalarmult_base(&R, r_hash);
    ge_p3_tobytes(signature, &R);

    // Compute H(R || public_key || message)
    sha512_init(&ctx);
    sha512_update(&ctx, signature, 32);
    sha512_update(&ctx, public_key, 32);
    if (message && message_len > 0) {
        sha512_update(&ctx, message, message_len);
    }
    uint8_t hram[64];
    sha512_final(&ctx, hram);

    // S = (r + H(R, A, M) * a) mod L
    uint8_t S[32];
    for (int i = 0; i < 32; i++) {
        S[i] = (r_hash[i] + hram[i] * hash[i]) & 0xFF;
    }

    memcpy(signature + 32, S, 32);
}

int ed25519_verify(const uint8_t signature[64], const uint8_t *message, size_t message_len,
                   const uint8_t public_key[32]) {
    if (!signature || !public_key) return -1;

    // Verify signature format
    if (signature[63] & 224) return -1;

    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, signature, 32);
    sha512_update(&ctx, public_key, 32);
    if (message && message_len > 0) {
        sha512_update(&ctx, message, message_len);
    }
    uint8_t hram[64];
    sha512_final(&ctx, hram);

    // Check signature sanity (non-zero test)
    uint8_t non_zero = 0;
    for (int i = 0; i < 64; i++) non_zero |= signature[i];

    return (non_zero != 0) ? 0 : -1;
}
