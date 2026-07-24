#include "test.h"
#include "kprintf.h"
#include "kstring.h"
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

TEST(memory_routines) {
    static uint8_t a[300], b[300];

    // Every length from 0 to 40 at several alignments: the 8-byte and the
    // single-byte parts of each routine both get used, and nothing may be
    // written outside the requested range.
    for (size_t align = 0; align < 9; align++) {
        for (size_t len = 0; len <= 40; len++) {
            memset(a, 0xEE, sizeof(a));
            memset(a + 64 + align, 0x5A, len);
            int ok = (a[63 + align] == 0xEE) && (a[64 + align + len] == 0xEE);
            for (size_t i = 0; i < len; i++) ok &= (a[64 + align + i] == 0x5A);

            for (size_t i = 0; i < sizeof(b); i++) b[i] = (uint8_t)(i * 7 + 1);
            memset(a, 0xEE, sizeof(a));
            memcpy(a + 64 + align, b + 3, len);
            ok &= (a[63 + align] == 0xEE) && (a[64 + align + len] == 0xEE);
            ok &= (memcmp(a + 64 + align, b + 3, len) == 0);
            EXPECT_TRUE(ok);
        }
    }
    EXPECT_TRUE(memset(a, 1, 0) == a && memcpy(a, b, 0) == a);

    // memmove with overlap in both directions, including by one byte
    for (size_t shift = 1; shift <= 17; shift += 4) {
        for (size_t i = 0; i < sizeof(a); i++) a[i] = (uint8_t)i;
        EXPECT_TRUE(memmove(a + shift, a, 200) == a + shift);    // Destination above source
        int ok = 1;
        for (size_t i = 0; i < 200; i++) ok &= (a[shift + i] == (uint8_t)i);
        ok &= (a[shift + 200] == (uint8_t)(shift + 200));        // Nothing past the end
        EXPECT_TRUE(ok);

        for (size_t i = 0; i < sizeof(a); i++) a[i] = (uint8_t)i;
        memmove(a, a + shift, 200);                              // Destination below source
        ok = 1;
        for (size_t i = 0; i < 200; i++) ok &= (a[i] == (uint8_t)(i + shift));
        ok &= (a[200] == 200);
        EXPECT_TRUE(ok);
    }

    // The direction flag is clear again after a backwards memmove
    uint64_t rflags;
    asm volatile("pushfq\n\tpop %0" : "=r"(rflags));
    EXPECT_TRUE((rflags & (1 << 10)) == 0);
}

TEST(string_search) {
    const char *text = "one two three two";
    EXPECT_TRUE(strstr(text, "two") == text + 4);          // The first match
    EXPECT_TRUE(strstr(text, "three two") == text + 8);
    EXPECT_TRUE(strstr(text, "twoo") == NULL);
    EXPECT_TRUE(strstr(text, "one two three two!") == NULL); // Longer than the text
    EXPECT_TRUE(strstr(text, "") == text);
    EXPECT_TRUE(strstr("", "a") == NULL);
    EXPECT_TRUE(strstr("aab", "ab") != NULL);              // Needs to back up after a partial match

    EXPECT_TRUE(strchr(text, 't') == text + 4);
    EXPECT_TRUE(strchr(text, 'z') == NULL);
    EXPECT_TRUE(strchr(text, '\0') == text + 17);
}

TEST(ksnprintf_formats_and_truncates) {
    char buf[64];

    EXPECT_EQ(ksnprintf(buf, sizeof(buf), "%d %u %x %s %c%%", -42, 42u, 0xBEEFu, "str", 'z'), 18u);
    EXPECT_EQ(strcmp(buf, "-42 42 beef str z%"), 0);

    // 64-bit arguments with z and l
    EXPECT_EQ(ksnprintf(buf, sizeof(buf), "%zu %lu %lx", (size_t)5000000000ULL,
                        (uint64_t)18446744073709551615ULL, (uint64_t)0x123456789ABCULL), 44u);
    EXPECT_EQ(strcmp(buf, "5000000000 18446744073709551615 123456789abc"), 0);
    ksnprintf(buf, sizeof(buf), "%ld", (int64_t)(-9223372036854775807LL - 1));
    EXPECT_EQ(strcmp(buf, "-9223372036854775808"), 0);   // The most negative value
    ksnprintf(buf, sizeof(buf), "%d|%s|%p", 0, (const char *)NULL, (void *)0x1000);
    EXPECT_EQ(strcmp(buf, "0|(null)|0x1000"), 0);

    // Field widths and zero padding
    ksnprintf(buf, sizeof(buf), "%02d:%02d|%5d|%05d|%3d", 7, 42, 123, -42, 12345);
    EXPECT_EQ(strcmp(buf, "07:42|  123|-0042|12345"), 0);   // A wide value is never cut
    ksnprintf(buf, sizeof(buf), "%08x|%4x|%6s|%2s|%04lu", 0xBEEFu, 0xFu, "ab", "long", (uint64_t)7);
    EXPECT_EQ(strcmp(buf, "0000beef|   f|    ab|long|0007"), 0);
    EXPECT_EQ(ksnprintf(buf, sizeof(buf), "%999999d", 1), 63u); // Width is bounded; output fits the buffer

    // Too long: cut off, still terminated, and nothing written past the end
    char small[8];
    memset(small, 'X', sizeof(small));
    EXPECT_EQ(ksnprintf(small, 5, "abcdefghij"), 4u);
    EXPECT_EQ(strcmp(small, "abcd"), 0);
    EXPECT_EQ(small[5], 'X');

    EXPECT_EQ(ksnprintf(small, 1, "abc"), 0u);
    EXPECT_EQ(small[0], '\0');
    EXPECT_EQ(ksnprintf(small, 0, "abc"), 0u);           // Size 0 touches nothing
    EXPECT_EQ(small[0], '\0');
}

void test_suite_lib(void) {
    kprintf("Suite: Core Standard Library & Crypto (lib/)\n");
    RUN_TEST(lib_crypto_hashing_cipher_test);
    RUN_TEST(lib_string_ctype_test);
    RUN_TEST(memory_routines);
    RUN_TEST(string_search);
    RUN_TEST(ksnprintf_formats_and_truncates);
}
