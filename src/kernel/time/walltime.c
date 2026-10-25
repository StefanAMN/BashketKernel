/**
 * @file walltime.c
 * @brief The time of day (see walltime.h)
 */

#include "walltime.h"
#include "hrtimer.h"
#include "kerrno.h"
#include "kprintf.h"
#include "spinlock.h"
#include "time/date.h"

#define NS_PER_SECOND 1000000000ULL

// The time of day is clock_ns() + offset_ns. Written at boot and by
// walltime_set, read everywhere. One aligned word, read and written whole
// (atomically): no lock is needed, and none could be taken around clock_ns,
// which has a lock of the same rank.
static uint64_t offset_ns;
static bool started = false;

// What walltime_set made the time of day differ from the CMOS clock by (0
// until someone sets it), and when (clock_ns) it was last compared with it
static int64_t set_delta_ns;
static uint64_t last_check_ns;

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

// The time the CMOS clock stands for: it counts whole seconds, so the time
// is somewhere in the second after its reading; the middle of it is never
// more than half a second out. 0 if the clock holds no date.
static uint64_t cmos_middle_ns(void) {
    uint64_t cmos = cmos_ns();
    return cmos ? cmos + NS_PER_SECOND / 2 : 0;
}

int64_t walltime_check(void) {
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return 0;
    uint64_t cmos = cmos_middle_ns();
    if (!cmos) return 0;
    uint64_t target = cmos + (uint64_t)__atomic_load_n(&set_delta_ns, __ATOMIC_ACQUIRE);
    uint64_t now = clock_ns();
    int64_t out = (int64_t)(now + __atomic_load_n(&offset_ns, __ATOMIC_ACQUIRE) - target);
    if (out > (int64_t)WALLTIME_CHECK_LIMIT_NS || out < -(int64_t)WALLTIME_CHECK_LIMIT_NS) {
        __atomic_store_n(&offset_ns, target - now, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&last_check_ns, now, __ATOMIC_RELEASE);
    return out;
}

// walltime_check, if a second has passed since the last one and this CPU
// holds no spinlock (the CMOS clock's lock ranks below many); one CPU at a
// time does it, the others carry on with the time as it is
static void check_if_due(void) {
    uint64_t now = clock_ns();
    uint64_t last = __atomic_load_n(&last_check_ns, __ATOMIC_ACQUIRE);
    if (now - last < NS_PER_SECOND || lock_debug_held_count() != 0) return;
    if (!__atomic_compare_exchange_n(&last_check_ns, &last, now, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    walltime_check();
}

void walltime_test_skew(int64_t delta_ns) {
    __atomic_add_fetch(&offset_ns, (uint64_t)delta_ns, __ATOMIC_ACQ_REL);
}

void walltime_init(void) {
    uint64_t cmos = cmos_ns();
    __atomic_store_n(&offset_ns, cmos - clock_ns(), __ATOMIC_RELEASE);
    __atomic_store_n(&last_check_ns, clock_ns(), __ATOMIC_RELEASE);
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
    check_if_due();
    return clock_ns() + __atomic_load_n(&offset_ns, __ATOMIC_ACQUIRE);
}

uint32_t walltime_seconds(void) {
    return (uint32_t)(walltime_ns() / NS_PER_SECOND);
}

int walltime_set(uint64_t unix_ns) {
    uint64_t seconds = unix_ns / NS_PER_SECOND;
    if (seconds < WALLTIME_MIN_SECONDS || seconds > WALLTIME_MAX_SECONDS) return -EINVAL;
    __atomic_store_n(&offset_ns, unix_ns - clock_ns(), __ATOMIC_RELEASE);
    uint64_t cmos = cmos_middle_ns();
    if (cmos) __atomic_store_n(&set_delta_ns, (int64_t)(unix_ns - cmos), __ATOMIC_RELEASE);   // Kept from now on
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);
    return 0;
}
