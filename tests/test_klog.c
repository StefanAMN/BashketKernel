#include "test.h"
#include "klog.h"
#include "file.h"
#include "kstring.h"

// Does the log currently end with `text`?
static bool log_ends_with(const char *text) {
    char tail[64];
    size_t len = strlen(text);
    size_t held = klog_length();
    if (held < len) return false;
    return klog_read(held - len, tail, len) == len && memcmp(tail, text, len) == 0;
}

// What kprintf prints is in the log, and reads stop at its end
TEST(klog_records_kernel_messages) {
    // Everything kprintf prints lands in the log, including this
    kprintf("klog-test-marker-%d\n", 12345);
    EXPECT_TRUE(log_ends_with("klog-test-marker-12345\n"));

    // The log starts with the first message of the boot (unless the boot
    // printed more than the buffer holds, which the test kernel does)
    EXPECT_TRUE(klog_length() > 0 && klog_length() <= KLOG_SIZE);

    // Reads stop at the end, and past it there is nothing
    char buf[16];
    size_t held = klog_length();
    EXPECT_EQ(klog_read(held - 4, buf, sizeof(buf)), 4u);
    EXPECT_EQ(klog_read(held, buf, sizeof(buf)), 0u);
    EXPECT_EQ(klog_read(held + 1000, buf, sizeof(buf)), 0u);
}

// A log written past its size keeps the newest text, in order
TEST(klog_keeps_the_newest_text_when_full) {
    // Write more than the buffer holds: 3000 numbered 32-byte records
    char record[40];
    for (int i = 0; i < 3000; i++) {
        ksnprintf(record, sizeof(record), "record-%08d-padding-to-32b.\n", i);
        klog_write(record);
    }
    EXPECT_EQ(klog_length(), (size_t)KLOG_SIZE);

    // The newest record is at the end ...
    EXPECT_TRUE(log_ends_with("record-00002999-padding-to-32b.\n"));

    // ... the oldest records are gone, and what is left is in order: the
    // log begins part-way through some record and continues from there
    char head[80];
    EXPECT_EQ(klog_read(0, head, sizeof(head) - 1), sizeof(head) - 1);
    head[sizeof(head) - 1] = '\0';
    const char *first = strstr(head, "record-");
    EXPECT_TRUE(first != NULL);
    if (first) {
        int number = 0;
        for (int i = 7; i < 15; i++) number = number * 10 + (first[i] - '0');
        EXPECT_TRUE(number > 0 && number < 2999);          // Record 0 was overwritten
        char expected_next[40];
        ksnprintf(expected_next, sizeof(expected_next), "record-%08d", number + 1);
        EXPECT_TRUE(strstr(first + 7, expected_next) != NULL);
    }
}

// /proc/kmsg gives the log, read in odd-sized pieces
TEST(klog_is_readable_as_a_file) {
    kprintf("kmsg-file-marker\n");

    file_t *file = NULL;
    EXPECT_EQ(file_open("/proc/kmsg", O_RDONLY, &file), 0);

    // Read it all in odd-sized pieces and keep the last bytes seen
    char tail[32] = {0};
    char chunk[100];
    size_t total = 0;
    int64_t n;
    while ((n = file_read(file, chunk, sizeof(chunk))) > 0) {
        total += (size_t)n;
        if (n >= 17) memcpy(tail, chunk + n - 17, 17);
        else { memmove(tail, tail + n, 17 - (size_t)n); memcpy(tail + 17 - n, chunk, (size_t)n); }
    }
    file_close(file);

    EXPECT_TRUE(total >= 17 && total <= KLOG_SIZE);
    EXPECT_EQ(memcmp(tail, "kmsg-file-marker\n", 17), 0);
}

void test_suite_klog(void) {
    kprintf("Suite: Kernel Message Log\n");
    RUN_TEST(klog_records_kernel_messages);
    RUN_TEST(klog_keeps_the_newest_text_when_full);
    RUN_TEST(klog_is_readable_as_a_file);
}
