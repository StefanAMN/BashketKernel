#include "test.h"
#include "dev.h"
#include "vfs.h"
#include "file.h"
#include "kstring.h"

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

void test_suite_dev(void) {
    kprintf("Suite: /dev\n");
    RUN_TEST(dev_null_and_zero);
    RUN_TEST(dev_random_varies);
}
