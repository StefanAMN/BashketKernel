/**
 * @file walltime.c
 * @brief The time of day (see walltime.h)
 */

#include "walltime.h"
#include "hrtimer.h"
#include "kerrno.h"
#include "kprintf.h"
#include "time/date.h"

#define NS_PER_SECOND 1000000000ULL

// The time of day is clock_ns() + offset_ns. Written at boot and by
// walltime_set, read everywhere. One aligned word, read and written whole
// (atomically): no lock is needed, and none could be taken around clock_ns,
// which has a lock of the same rank.
static uint64_t offset_ns;
static bool started = false;

uint64_t walltime_from_date(const struct rtc_time *date) {
    int64_t days = days_from_civil(date->year, date->month, date->day);
    return (uint64_t)(days * 86400 + date->hour * 3600 + date->minute * 60 + date->second);
}

void walltime_to_date(uint64_t seconds, struct rtc_time *out) {
    unsigned year, month, day;
    civil_from_days((int64_t)(seconds / 86400), &year, &month, &day);
    uint32_t rest = (uint32_t)(seconds % 86400);
    out->year = (uint16_t)year;
    out->month = (uint8_t)month;
    out->day = (uint8_t)day;
    out->hour = (uint8_t)(rest / 3600);
    out->minute = (uint8_t)(rest / 60 % 60);
    out->second = (uint8_t)(rest % 60);
}

// The CMOS clock as Unix time in nanoseconds; 0 if what it holds is not a
// date (a clock that was never set)
static uint64_t cmos_ns(void) {
    struct rtc_time now;
    rtc_read_time(&now);
    if (now.month < 1 || now.month > 12 || now.day < 1 || now.day > 31 || now.hour > 23 ||
        now.minute > 59 || now.second > 59) return 0;
    return walltime_from_date(&now) * NS_PER_SECOND;
}

void walltime_init(void) {
    uint64_t cmos = cmos_ns();
    __atomic_store_n(&offset_ns, cmos - clock_ns(), __ATOMIC_RELEASE);
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);

    struct rtc_time date;
    walltime_to_date(cmos / NS_PER_SECOND, &date);
    kprintf("walltime: %u-%02u-%02u %02u:%02u:%02u UTC, from the CMOS clock\n",
            date.year, date.month, date.day, date.hour, date.minute, date.second);
}

uint64_t walltime_ns(void) {
    // Before walltime_init the nanosecond clock is not calibrated yet (it
    // starts from zero again when it is): ask the CMOS clock itself
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return cmos_ns();
    return clock_ns() + __atomic_load_n(&offset_ns, __ATOMIC_ACQUIRE);
}

uint32_t walltime_seconds(void) {
    return (uint32_t)(walltime_ns() / NS_PER_SECOND);
}

int walltime_set(uint64_t unix_ns) {
    uint64_t seconds = unix_ns / NS_PER_SECOND;
    if (seconds < WALLTIME_MIN_SECONDS || seconds > WALLTIME_MAX_SECONDS) return -EINVAL;
    __atomic_store_n(&offset_ns, unix_ns - clock_ns(), __ATOMIC_RELEASE);
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);
    return 0;
}
