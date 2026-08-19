#include "test.h"
#include "rtc.h"
#include "walltime.h"
#include "hrtimer.h"
#include "file.h"
#include "kerrno.h"
#include "kstring.h"

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

// The calendar arithmetic, against Unix times computed by Python's
// calendar.timegm: a leap day, the ends of the range the kernel accepts,
// and the turn of a century
TEST(dates_and_unix_times_agree) {
    static const struct { uint16_t y; uint8_t mo, d, h, mi, s; uint64_t seconds; } cases[] = {
        { 1970, 1, 1, 0, 0, 0, 0 },
        { 1980, 1, 1, 0, 0, 0, 315532800 },
        { 1999, 12, 31, 23, 59, 59, 946684799 },
        { 2000, 2, 29, 12, 34, 56, 951827696 },
        { 2026, 10, 20, 0, 0, 0, 1792454400 },
        { 2105, 12, 31, 23, 59, 59, 4291747199ULL },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct rtc_time t = { .year = cases[i].y, .month = cases[i].mo, .day = cases[i].d,
                              .hour = cases[i].h, .minute = cases[i].mi, .second = cases[i].s };
        EXPECT_TRUE(walltime_from_date(&t) == cases[i].seconds);
        struct rtc_time back;
        walltime_to_date(cases[i].seconds, &back);
        EXPECT_TRUE(back.year == t.year && back.month == t.month && back.day == t.day &&
                    back.hour == t.hour && back.minute == t.minute && back.second == t.second);
    }
}

// The time of day is the CMOS clock's, kept by the nanosecond clock
TEST(time_of_day_follows_the_cmos_clock) {
    struct rtc_time cmos;
    rtc_read_time(&cmos);
    uint64_t cmos_seconds = walltime_from_date(&cmos);
    uint64_t ours = walltime_seconds();
    EXPECT_TRUE(ours + 2 >= cmos_seconds && ours <= cmos_seconds + 2);   // Both count whole seconds

    uint64_t before = walltime_ns(), clock_before = clock_ns();
    nanosleep(30000000);                                                  // 30 ms
    uint64_t passed = walltime_ns() - before, clock_passed = clock_ns() - clock_before;
    EXPECT_TRUE(passed >= 30000000 && passed + 1000000 >= clock_passed && passed <= clock_passed + 1000000);
}

// Setting the time moves it, and what is written from then on is dated by
// it; out of range is refused. The time is put back afterwards.
TEST(time_of_day_can_be_set) {
    uint64_t saved = walltime_ns(), saved_at = clock_ns();
    EXPECT_EQ(walltime_set(100ULL * 1000000000ULL), -EINVAL);                       // 1970
    EXPECT_EQ(walltime_set((WALLTIME_MAX_SECONDS + 1) * 1000000000ULL), -EINVAL);  // 2106

    const uint64_t y2030 = 1893456000ULL;                                         // 2030-01-01 00:00:00
    EXPECT_EQ(walltime_set(y2030 * 1000000000ULL), 0);
    EXPECT_TRUE(walltime_seconds() >= y2030 && walltime_seconds() <= y2030 + 1);

    // A file written now carries the new date
    file_t *file = NULL;
    EXPECT_EQ(file_open("/tmp/dated", O_WRONLY | O_CREAT, &file), 0);
    vnode_t *root_file = NULL;
    if (vfs_get_root()->ops) {
        // On the system disk too, which stores it
        file_t *on_disk = NULL;
        EXPECT_EQ(file_open("/dated-on-disk", O_WRONLY | O_CREAT, &on_disk), 0);
        if (on_disk) {
            EXPECT_EQ(file_write(on_disk, "x", 1), 1);
            root_file = on_disk->node;
            EXPECT_TRUE(root_file->mtime >= y2030 && root_file->mtime <= y2030 + 2);
            file_close(on_disk);
        }
        EXPECT_EQ(vfs_unlink(NULL, "dated-on-disk"), 0);
    }
    if (file) file_close(file);
    vfs_unlink(vfs_find(NULL, "/tmp"), "dated");

    EXPECT_EQ(walltime_set(saved + (clock_ns() - saved_at)), 0);
    uint64_t now = walltime_ns();
    EXPECT_TRUE(now >= saved && now - saved < 5000000000ULL);
}

void test_suite_rtc(void) {
    kprintf("Suite: The CMOS clock and the time of day\n");
    RUN_TEST(rtc_time_read_test);
    RUN_TEST(rtc_decodes_every_format);
    RUN_TEST(dates_and_unix_times_agree);
    RUN_TEST(time_of_day_follows_the_cmos_clock);
    RUN_TEST(time_of_day_can_be_set);
}
