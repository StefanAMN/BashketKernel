#include "test.h"
#include "crypto/chacha20.h"
#include "random.h"
#include "dev.h"
#include "vfs.h"
#include "file.h"
#include "kstring.h"

// /dev/null swallows writes and reads as empty; /dev/zero gives zeros without end
TEST(dev_null_and_zero) {
    file_t *file = NULL;
    uint8_t buf[64];

    EXPECT_EQ(file_open("/dev/null", O_RDWR, &file), 0);
    EXPECT_EQ(file_write(file, "thrown away", 11), 11);
    EXPECT_EQ(file_read(file, buf, sizeof(buf)), 0);      // Always end-of-file
    file_close(file);
    EXPECT_EQ(vfs_find(NULL, "/dev/null")->size, 0u);     // Nothing was stored

    EXPECT_EQ(file_open("/dev/zero", O_RDONLY, &file), 0);
    memset(buf, 0xFF, sizeof(buf));
    EXPECT_EQ(file_read(file, buf, sizeof(buf)), (int64_t)sizeof(buf));
    int nonzero = 0;
    for (size_t i = 0; i < sizeof(buf); i++) nonzero |= buf[i];
    EXPECT_EQ(nonzero, 0);
    EXPECT_EQ(file_read(file, buf, sizeof(buf)), (int64_t)sizeof(buf)); // Never runs out
    file_close(file);
}

// /dev/random differs between reads, has its bits about half set, and fills
// exactly the length asked for
TEST(dev_random_varies) {
    file_t *file = NULL;
    uint8_t a[32], b[32];

    EXPECT_EQ(file_open("/dev/random", O_RDONLY, &file), 0);
    EXPECT_EQ(file_read(file, a, sizeof(a)), (int64_t)sizeof(a));
    EXPECT_EQ(file_read(file, b, sizeof(b)), (int64_t)sizeof(b));
    file_close(file);
    EXPECT_TRUE(memcmp(a, b, sizeof(a)) != 0);

    // A crude sanity check, not a statistical test: over 4096 bytes every
    // bit position should be set somewhere between 30% and 70% of the time.
    static uint8_t sample[4096];
    get_random_bytes(sample, sizeof(sample));
    for (int bit = 0; bit < 8; bit++) {
        int ones = 0;
        for (size_t i = 0; i < sizeof(sample); i++) ones += (sample[i] >> bit) & 1;
        EXPECT_TRUE(ones > 1228 && ones < 2868);
    }

    // Odd lengths fill exactly what was asked for
    uint8_t guarded[8];
    memset(guarded, 0x5A, sizeof(guarded));
    get_random_bytes(guarded, 3);
    EXPECT_TRUE(guarded[3] == 0x5A && guarded[7] == 0x5A);
}

// `count` bytes from lower-case hex digits (test vectors, trusted)
static void from_hex(const char *hex, uint8_t *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t v = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[2 * i + j];
            v = (uint8_t)((v << 4) | (c <= '9' ? c - '0' : c - 'a' + 10));
        }
        out[i] = v;
    }
}

// The generator rests on ChaCha20 being ChaCha20: the keystream for the test
// vectors of RFC 8439 (sections 2.3.2 and A.1), which agree with an
// independent implementation.
TEST(chacha20_known_answers) {
    uint8_t key[32], nonce[12], zeros[64] = {0}, stream[64], expected[64];

    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    from_hex("000000090000004a00000000", nonce, 12);
    from_hex("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
             "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", expected, 64);
    chacha20_encrypt(key, nonce, 1, zeros, stream, 64);
    EXPECT_EQ(memcmp(stream, expected, 64), 0);

    memset(key, 0, sizeof(key));
    memset(nonce, 0, sizeof(nonce));
    from_hex("76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
             "da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586", expected, 64);
    chacha20_encrypt(key, nonce, 0, zeros, stream, 64);
    EXPECT_EQ(memcmp(stream, expected, 64), 0);
}

// One step of the generator: the keystream's first 32 bytes become the next
// key and are never output; what is output follows them
TEST(random_step_is_chacha20_with_the_key_replaced) {
    uint8_t key[32], next[32], out[64], expected_key[32], expected_out[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    from_hex("f84e3face34bb5941aab01082aadad99cd9183cf6126bf90fdb2aeb1fcacd9f2", expected_key, 32);
    from_hex("4465f80b116c4150e7275f3e0ddb0d88c4628c07af6a852503ec90e43363ba79", expected_out, 32);

    random_expand(key, 5, next, out, 32);
    EXPECT_EQ(memcmp(next, expected_key, 32), 0);
    EXPECT_EQ(memcmp(out, expected_out, 32), 0);
    EXPECT_TRUE(memcmp(next, out, 32) != 0);

    // Another request number, another stream; and the same step again from
    // the new key gives something new: no output ever repeats
    uint8_t other[32], again[32], next2[32];
    random_expand(key, 6, next2, other, 32);
    EXPECT_TRUE(memcmp(other, out, 32) != 0);
    random_expand(next, 5, next2, again, 32);
    EXPECT_TRUE(memcmp(again, out, 32) != 0);
}

// Not a proof of anything (a bad generator can pass these), but a broken
// one does not: bits balanced, byte values evenly spread, no block twice.
TEST(random_output_looks_random) {
    enum { BYTES = 65536 };
    static uint8_t sample[BYTES];
    static uint32_t counts[256];
    get_random_bytes(sample, BYTES);

    uint64_t ones = 0;
    memset(counts, 0, sizeof(counts));
    for (size_t i = 0; i < BYTES; i++) {
        counts[sample[i]]++;
        for (int bit = 0; bit < 8; bit++) ones += (sample[i] >> bit) & 1;
    }
    // 524288 bits: expect half, standard deviation 362
    EXPECT_TRUE(ones > 262144 - 2200 && ones < 262144 + 2200);

    // Each byte value expected 256 times. The chi-square statistic has mean
    // 255 and standard deviation about 23 for random data.
    uint64_t chi_times_256 = 0;
    for (int v = 0; v < 256; v++) {
        int64_t d = (int64_t)counts[v] - 256;
        chi_times_256 += (uint64_t)(d * d);
    }
    uint64_t chi = chi_times_256 / 256;
    EXPECT_TRUE(chi > 150 && chi < 400);

    // No 8-byte block equal to the one 8, 16, ... bytes before it
    int repeats = 0;
    for (size_t i = 8; i + 8 <= BYTES; i += 8) {
        if (memcmp(sample + i, sample + i - 8, 8) == 0) repeats++;
    }
    EXPECT_EQ(repeats, 0);

    // Requests of awkward sizes are filled exactly, and not with one pattern
    uint8_t small[7] = {0}, big[1000];
    memset(big, 0xAA, sizeof(big));
    get_random_bytes(small, sizeof(small));
    get_random_bytes(big, 999);
    EXPECT_TRUE(big[999] == 0xAA);                               // Not a byte too many
    int still_aa = 0;
    for (int i = 0; i < 999; i++) still_aa += big[i] == 0xAA;
    EXPECT_TRUE(still_aa < 30);
    EXPECT_TRUE(get_random_u32() != get_random_u32());
}

// Interrupt timings are collected and folded into the key now and then
TEST(random_reseeds_from_events) {
    uint8_t byte;
    // Start from an empty collection: enough events to force a reseed, and
    // the request that performs it
    for (int i = 0; i < 40; i++) random_add_event((uint64_t)i);
    get_random_bytes(&byte, 1);
    uint64_t before = random_reseed_count();
    EXPECT_TRUE(before >= 1);                                    // It was seeded at boot

    for (int i = 0; i < 10; i++) random_add_event((uint64_t)i);
    get_random_bytes(&byte, 1);
    EXPECT_TRUE(random_reseed_count() == before);                // Not enough new material yet
    for (int i = 0; i < 40; i++) random_add_event((uint64_t)i);
    get_random_bytes(&byte, 1);
    EXPECT_TRUE(random_reseed_count() == before + 1);

    const char *source = random_source();
    EXPECT_TRUE(strcmp(source, "RDSEED") == 0 || strcmp(source, "RDRAND") == 0 || strcmp(source, "timing only") == 0);
    kprintf("    entropy source on this CPU: %s\n", source);
}

void test_suite_dev(void) {
    kprintf("Suite: /dev\n");
    RUN_TEST(dev_null_and_zero);
    RUN_TEST(dev_random_varies);
    RUN_TEST(chacha20_known_answers);
    RUN_TEST(random_step_is_chacha20_with_the_key_replaced);
    RUN_TEST(random_output_looks_random);
    RUN_TEST(random_reseeds_from_events);
}
