#include "test.h"
#include "crypto.h"

TEST(crypto_sha256_aes_test) {
    uint8_t hash[32];
    sha256_hash((const uint8_t *)"BashketOS", 9, hash);
    EXPECT_TRUE(hash[0] != 0);

    uint8_t key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    uint8_t block[16] = "TestBlock123456";

    aes128_encrypt_block(key, block);
    EXPECT_TRUE(block[0] != 'T');
}

void test_suite_crypto(void) {
    kprintf("Suite: Phase 36 Cryptographic Primitives\n");
    RUN_TEST(crypto_sha256_aes_test);
}
