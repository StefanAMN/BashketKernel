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
    // NIST test vector: SHA-512("abc") = ddaf35a193617abacc417349ae204131...
    uint8_t hash[64];
    sha512_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0xdd);
    EXPECT_EQ(hash[1], 0xaf);
    EXPECT_EQ(hash[2], 0x35);
    EXPECT_EQ(hash[3], 0xa1);
}

TEST(crypto_sha1_test) {
    // NIST test vector: SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
    uint8_t hash[20];
    sha1_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0xa9);
    EXPECT_EQ(hash[1], 0x99);
    EXPECT_EQ(hash[2], 0x3e);
    EXPECT_EQ(hash[3], 0x36);
}

TEST(crypto_md5_test) {
    // RFC 1321 test vector: MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
    uint8_t hash[16];
    md5_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0x90);
    EXPECT_EQ(hash[1], 0x01);
    EXPECT_EQ(hash[2], 0x50);
    EXPECT_EQ(hash[15], 0x72);
}

TEST(crypto_blake2s_test) {
    // RFC 7693 test vector: BLAKE2s-256("abc") = 508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982
    uint8_t hash[32];
    blake2s_hash((const uint8_t *)"abc", 3, hash);

    EXPECT_EQ(hash[0], 0x50);
    EXPECT_EQ(hash[1], 0x8c);
    EXPECT_EQ(hash[2], 0x5e);
    EXPECT_EQ(hash[31], 0x82);
}

TEST(crypto_aes_test) {
    // FIPS 197 AES-128 test vector (all zeros key and block)
    uint8_t key128[16] = {0};
    uint8_t block128[16] = {0};
    aes128_encrypt_block(key128, block128);

    // Expected ciphertext: 66e94bd4ef8a2c3b884cfa59ca342b2e
    EXPECT_EQ(block128[0], 0x66);
    EXPECT_EQ(block128[1], 0xe9);
    EXPECT_EQ(block128[2], 0x4b);
    EXPECT_EQ(block128[15], 0x2e);

    // Decrypt back to all zeros
    aes128_decrypt_block(key128, block128);
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(block128[i], 0);
    }

    // FIPS 197 AES-256 test vector (all zeros key and block)
    uint8_t key256[32] = {0};
    uint8_t block256[16] = {0};
    aes256_encrypt_block(key256, block256);

    // Expected ciphertext: dc95c078a240898956121090741ee084
    EXPECT_EQ(block256[0], 0xdc);
    EXPECT_EQ(block256[1], 0x95);
    EXPECT_EQ(block256[2], 0xc0);
    EXPECT_EQ(block256[15], 0x84);

    aes256_decrypt_block(key256, block256);
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(block256[i], 0);
    }
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
    // RFC 4231 Test Case 2: key = "Jefe", data = "what do ya want for nothing?"
    // HMAC-SHA256 = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
    const uint8_t key[] = "Jefe";
    const uint8_t data[] = "what do ya want for nothing?";
    uint8_t mac[32];

    hmac_sha256(key, 4, data, 28, mac);
    EXPECT_EQ(mac[0], 0x5b);
    EXPECT_EQ(mac[1], 0xdc);
    EXPECT_EQ(mac[2], 0xc1);
    EXPECT_EQ(mac[31], 0x43);
}

TEST(crypto_crc32_test) {
    // Standard test vector: "123456789"
    // IEEE 802.3 CRC32("123456789") = 0xcbf43926
    // Castagnoli CRC32C("123456789") = 0xe3069283
    const uint8_t data[] = "123456789";
    uint32_t c32 = crc32_calc(data, 9);
    uint32_t c32c = crc32c_calc(data, 9);

    EXPECT_EQ(c32, 0xcbf43926);
    EXPECT_EQ(c32c, 0xe3069283);
}

void test_suite_crypto(void) {
    kprintf("Suite: Expanded Kernel Cryptographic Primitives\n");
    RUN_TEST(crypto_sha256_test);
    RUN_TEST(crypto_sha512_test);
    RUN_TEST(crypto_sha1_test);
    RUN_TEST(crypto_md5_test);
    RUN_TEST(crypto_blake2s_test);
    RUN_TEST(crypto_aes_test);
    RUN_TEST(crypto_chacha20_test);
    RUN_TEST(crypto_poly1305_test);
    RUN_TEST(crypto_hmac_test);
    RUN_TEST(crypto_crc32_test);
}
