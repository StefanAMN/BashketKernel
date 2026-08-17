#include "test.h"
#include "crypto.h"
#include "string.h"
#include "ctype.h"

TEST(lib_crypto_hashing_cipher_test) {
    uint8_t sha_hash[32];
    sha256_hash((const uint8_t *)"BashketOS Kernel", 16, sha_hash);
    EXPECT_TRUE(sha_hash[0] != 0);

    uint8_t md5_out[16];
    md5_hash((const uint8_t *)"BashketOS Kernel", 16, md5_out);
    EXPECT_TRUE(md5_out[0] != 0);

    uint8_t key[16] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x00};
    uint8_t block[16] = "SecretDataBlock";
    uint8_t original[16];
    memcpy(original, block, 16);

    // Encrypt
    aes128_encrypt_block(key, block);
    EXPECT_TRUE(memcmp(original, block, 16) != 0);

    // Decrypt
    aes128_decrypt_block(key, block);
    EXPECT_TRUE(memcmp(original, block, 16) == 0);
}

TEST(lib_string_ctype_test) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    strcpy(buf, "Hello ");
    strcat(buf, "World");
    EXPECT_TRUE(strcmp(buf, "Hello World") == 0);
    EXPECT_TRUE(strlen(buf) == 11);

    EXPECT_TRUE(isdigit('5'));
    EXPECT_TRUE(isalpha('A'));
    EXPECT_TRUE(toupper('a') == 'A');
    EXPECT_TRUE(tolower('B') == 'b');
}

void test_suite_lib(void) {
    kprintf("Suite: Core Standard Library & Crypto (lib/)\n");
    RUN_TEST(lib_crypto_hashing_cipher_test);
    RUN_TEST(lib_string_ctype_test);
}
