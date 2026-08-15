#include "test.h"
#include "rtc.h"

TEST(rtc_time_read_test) {
    struct rtc_time t;
    rtc_read_time(&t);

    EXPECT_TRUE(t.year >= 2020);
    EXPECT_TRUE(t.month >= 1 && t.month <= 12);
    EXPECT_TRUE(t.day >= 1 && t.day <= 31);
}

// The formats a clock may keep its registers in (status register B: bit 1
// 24-hour mode, bit 2 binary rather than BCD)
TEST(rtc_decodes_every_format) {
    struct rtc_time t;
    const uint8_t bcd_24[6] = { 0x59, 0x30, 0x23, 0x31, 0x12, 0x26 };
    rtc_decode(bcd_24, 0x02, &t);
    EXPECT_TRUE(t.second == 59 && t.minute == 30 && t.hour == 23 && t.day == 31 && t.month == 12 && t.year == 2026);
    const uint8_t binary_24[6] = { 59, 30, 23, 31, 12, 26 };
    rtc_decode(binary_24, 0x06, &t);
    EXPECT_TRUE(t.second == 59 && t.hour == 23 && t.year == 2026);

    // 12-hour mode: bit 7 of the hour is PM; 12 AM is midnight, 12 PM noon
    uint8_t h12[6] = { 0, 0, 0x12, 1, 1, 0x26 };
    rtc_decode(h12, 0x00, &t);
    EXPECT_EQ(t.hour, 0);                                  // 12 AM
    h12[2] = 0x92;
    rtc_decode(h12, 0x00, &t);
    EXPECT_EQ(t.hour, 12);                                 // 12 PM
    h12[2] = 0x81;
    rtc_decode(h12, 0x00, &t);
    EXPECT_EQ(t.hour, 13);                                 // 1 PM
    h12[2] = 0x80 | 11;
    rtc_decode(h12, 0x04, &t);                             // Binary, 12-hour: 11 PM
    EXPECT_EQ(t.hour, 23);
}

void test_suite_rtc(void) {
    kprintf("Suite: Phase 17 Real-Time Clock & CMOS Driver\n");
    RUN_TEST(rtc_time_read_test);
    RUN_TEST(rtc_decodes_every_format);
}
