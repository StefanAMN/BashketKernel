/**
 * @file test_crypto.c
 * @brief SHA-256 against published test vectors
 *
 * SHA-256 is what passwords are hashed with (PBKDF2, in /bin/login) and what
 * the random generator is rekeyed through. The vectors are FIPS 180-2's
 * ("abc", the two-block message, a million 'a's) and the empty message's,
 * whose hashes Python's hashlib gives too. (ChaCha20, the generator's other
 * half, is checked against RFC 8439 in test_dev.c.)
 */

#include "test.h"
#include "kstring.h"
#include "crypto/sha256.h"

// The 32 bytes written as 64 hex digits
static void to_hex(const uint8_t hash[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = digits[hash[i] >> 4];
        out[2 * i + 1] = digits[hash[i] & 15];
    }
    out[64] = '\0';
}

TEST(sha256_known_answers) {
    uint8_t hash[32];
    char hex[65];

    sha256_hash((const uint8_t *)"abc", 3, hash);
    to_hex(hash, hex);
    EXPECT_EQ(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), 0);

    sha256_hash((const uint8_t *)"", 0, hash);
    to_hex(hash, hex);
    EXPECT_EQ(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), 0);

    // 56 bytes: the padding no longer fits in the block, and needs a second
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hash((const uint8_t *)two_blocks, strlen(two_blocks), hash);
    to_hex(hash, hex);
    EXPECT_EQ(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), 0);
}

// Fed in pieces of awkward sizes, the hash is the same as in one go: PBKDF2
// and the generator both feed it that way
TEST(sha256_in_pieces) {
    static uint8_t message[1000];
    memset(message, 'a', sizeof(message));
    uint8_t whole[32], pieces[32];
    char hex[65];

    sha256_hash(message, sizeof(message), whole);
    to_hex(whole, hex);
    EXPECT_EQ(strcmp(hex, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"), 0);

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    static const size_t sizes[] = { 1, 63, 64, 65, 7, 300, 500 };   // 1000 in all
    size_t at = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        sha256_update(&ctx, message + at, sizes[i]);
        at += sizes[i];
    }
    sha256_final(&ctx, pieces);
    EXPECT_EQ(at, sizeof(message));
    EXPECT_EQ(memcmp(whole, pieces, 32), 0);
}

void test_suite_crypto(void) {
    kprintf("Suite: SHA-256\n");
    RUN_TEST(sha256_known_answers);
    RUN_TEST(sha256_in_pieces);
}
