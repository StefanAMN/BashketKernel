/**
 * @file test_crypto.c
 * @brief Comprehensive Unit Test Suite for Kernel Cryptographic Primitives
 */

#include "test.h"
#include "crypto.h"
#include "string.h"

TEST(crypto_sha256_test) {
    // NIST test vector: SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    uint8_t hash[32];
    sha256_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0xba);
    EXPECT_EQ(hash[1], 0x78);
    EXPECT_EQ(hash[2], 0x16);
    EXPECT_EQ(hash[31], 0xad);
}

TEST(crypto_sha512_test) {
    uint8_t hash[64];
    sha512_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0xdd);
    EXPECT_EQ(hash[1], 0xaf);
    EXPECT_EQ(hash[2], 0x35);
    EXPECT_EQ(hash[3], 0xa1);
}

TEST(crypto_sha1_test) {
    uint8_t hash[20];
    sha1_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0xa9);
    EXPECT_EQ(hash[1], 0x99);
    EXPECT_EQ(hash[2], 0x3e);
    EXPECT_EQ(hash[3], 0x36);
}

TEST(crypto_md5_test) {
    uint8_t hash[16];
    md5_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0x90);
    EXPECT_EQ(hash[1], 0x01);
    EXPECT_EQ(hash[2], 0x50);
    EXPECT_EQ(hash[15], 0x72);
}

TEST(crypto_blake2s_test) {
    uint8_t hash[32];
    blake2s_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0x50);
    EXPECT_EQ(hash[1], 0x8c);
    EXPECT_EQ(hash[2], 0x5e);
    EXPECT_EQ(hash[31], 0x82);
}

TEST(crypto_siphash_test) {
    // SipHash-2-4 reference test vector for 15-byte message 0x00..0x0E with key 0x00..0x0F
    uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    uint8_t data[15];
    for (int i = 0; i < 15; i++) data[i] = (uint8_t)i;

    uint64_t hash = siphash24(key, data, 15);
    EXPECT_EQ(hash, 0xa129ca6149be45e5ULL);
}

TEST(crypto_aes_test) {
    uint8_t key128[16] = {0};
    uint8_t block128[16] = {0};
    aes128_encrypt_block(key128, block128);

    EXPECT_EQ(block128[0], 0x66);
    EXPECT_EQ(block128[1], 0xe9);
    EXPECT_EQ(block128[2], 0x4b);
    EXPECT_EQ(block128[15], 0x2e);

    aes128_decrypt_block(key128, block128);
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(block128[i], 0);
    }
}

TEST(crypto_aes_xts_test) {
    uint8_t key[32] = {0};
    uint8_t in[32] = "DiskSectorData1234567890123456";
    uint8_t out[32];
    uint8_t dec[32];

    aes128_xts_encrypt(key, 1, in, out, 32);
    EXPECT_TRUE(memcmp(in, out, 32) != 0);

    aes128_xts_decrypt(key, 1, out, dec, 32);
    EXPECT_TRUE(memcmp(in, dec, 32) == 0);
}

TEST(crypto_aes_gcm_test) {
    uint8_t key[16] = {0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08};
    uint8_t iv[12] = {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88};
    uint8_t aad[16] = "NetworkHeader12";
    uint8_t pt[16] = "SecretDataPacke";
    uint8_t ct[16];
    uint8_t tag[16];
    uint8_t dt[16];

    aes128_gcm_encrypt(key, iv, aad, 16, pt, ct, 16, tag);
    EXPECT_TRUE(memcmp(pt, ct, 16) != 0);

    int status = aes128_gcm_decrypt(key, iv, aad, 16, ct, dt, 16, tag);
    EXPECT_EQ(status, 0);
    EXPECT_TRUE(memcmp(pt, dt, 16) == 0);
}

TEST(crypto_chacha20_test) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t plaintext[16] = "Hello ChaCha20!";
    uint8_t ciphertext[16];
    uint8_t decrypted[16];

    chacha20_encrypt(key, nonce, 1, plaintext, ciphertext, 16);
    EXPECT_TRUE(memcmp(plaintext, ciphertext, 16) != 0);

    chacha20_encrypt(key, nonce, 1, ciphertext, decrypted, 16);
    EXPECT_TRUE(memcmp(plaintext, decrypted, 16) == 0);
}

TEST(crypto_poly1305_test) {
    uint8_t key[32] = {0};
    uint8_t msg[14] = "Test Poly1305";
    uint8_t mac[16];

    poly1305_mac(key, msg, 14, mac);
    EXPECT_TRUE(mac[0] != 0 || mac[1] != 0);
}

TEST(crypto_hmac_test) {
    const uint8_t key[] = "Jefe";
    const uint8_t data[] = "what do ya want for nothing?";
    uint8_t mac[32];

    hmac_sha256(key, 4, data, 28, mac);
    EXPECT_EQ(mac[0], 0x5b);
    EXPECT_EQ(mac[1], 0xdc);
    EXPECT_EQ(mac[2], 0xc1);
    EXPECT_EQ(mac[31], 0x43);
}

TEST(crypto_hkdf_test) {
    // RFC 5869 Test Case 1
    uint8_t ikm[22] = {0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b};
    uint8_t salt[13] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c};
    uint8_t info[10] = {0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9};
    uint8_t okm[42];

    hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, 42);

    // Expected OKM starts with 3cb743b916
    EXPECT_EQ(okm[0], 0x3c);
    EXPECT_EQ(okm[1], 0xb7);
    EXPECT_EQ(okm[2], 0x43);
    EXPECT_EQ(okm[3], 0xb9);
    EXPECT_EQ(okm[4], 0x16);
}

TEST(crypto_crc32_test) {
    const uint8_t data[] = "123456789";
    uint32_t c32 = crc32_calc(data, 9);
    uint32_t c32c = crc32c_calc(data, 9);

    EXPECT_EQ(c32, 0xcbf43926);
    EXPECT_EQ(c32c, 0xe3069283);
}

TEST(crypto_ed25519_test) {
    uint8_t seed[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    uint8_t pubkey[32];
    uint8_t privkey[64];
    uint8_t sig[64];
    uint8_t msg[] = "Hello World";

    ed25519_create_keypair(pubkey, privkey, seed);
    EXPECT_TRUE(pubkey[0] != 0 || pubkey[1] != 0);

    ed25519_sign(sig, msg, 11, pubkey, privkey);
    EXPECT_TRUE(sig[0] != 0 || sig[1] != 0);

    int res = ed25519_verify(sig, msg, 11, pubkey);
    EXPECT_EQ(res, 0);
}

void test_suite_crypto(void) {
    kprintf("Suite: Advanced Kernel Cryptographic Primitives\n");
    RUN_TEST(crypto_sha256_test);
    RUN_TEST(crypto_sha512_test);
    RUN_TEST(crypto_sha1_test);
    RUN_TEST(crypto_md5_test);
    RUN_TEST(crypto_blake2s_test);
    RUN_TEST(crypto_siphash_test);
    RUN_TEST(crypto_aes_test);
    RUN_TEST(crypto_aes_xts_test);
    RUN_TEST(crypto_aes_gcm_test);
    RUN_TEST(crypto_chacha20_test);
    RUN_TEST(crypto_poly1305_test);
    RUN_TEST(crypto_hmac_test);
    RUN_TEST(crypto_hkdf_test);
    RUN_TEST(crypto_crc32_test);
    RUN_TEST(crypto_ed25519_test);
}
