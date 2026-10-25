/**
 * @file walltime.h
 * @brief The time of day
 *
 * The CMOS clock (rtc.h) knows the date and time, to the second, but every
 * read is a dozen trips out of the virtual machine. The nanosecond clock
 * (hrtimer.h) is cheap and fine, but counts from boot. So the CMOS clock is
 * read once, as soon as the nanosecond clock runs, and the time of day is
 * from then on "what the CMOS clock said, plus how long ago that was".
 * Setting the time changes only that starting point: the CMOS clock itself
 * is not written (as with Linux's settimeofday; `hwclock` is another story).
 *
 * The nanosecond clock can fall behind (a CPU without a timestamp counter
 * counts ticks, and a host that is short of time loses some: a third of
 * them once on GitHub's runners) or run at a slightly wrong rate (the
 * counter's calibration). So the time of day is checked against the CMOS
 * clock, at most once a second when it is read (walltime_check), and put
 * right when it is more than WALLTIME_CHECK_LIMIT_NS out; a time someone
 * set is kept as its difference from the CMOS clock.
 *
 * Times are UTC, as nanoseconds or seconds since 1970-01-01 (Unix time). No
 * time zones, no leap seconds.
 */

#ifndef WALLTIME_H
#define WALLTIME_H

#include <stdint.h>
#include <stdbool.h>
#include "rtc.h"

// How far the time of day may drift from the CMOS clock (plus what was set)
// before it is put right: more than the second the CMOS clock rounds to
#define WALLTIME_CHECK_LIMIT_NS 1500000000ULL

// The range the kernel accepts for the time: FAT dates start in 1980, and
// ext2 keeps seconds in 32 bits, which run out in 2106
#define WALLTIME_MIN_SECONDS 315532800ULL    // 1980-01-01 00:00:00
#define WALLTIME_MAX_SECONDS 4291747199ULL   // 2105-12-31 23:59:59

/**
 * @brief Read the CMOS clock and start keeping the time from it. Call once,
 *        right after the nanosecond clock is calibrated (hrtimer_init).
 *        Until then walltime_ns reads the CMOS clock itself each time.
 */
void walltime_init(void);

/**
 * @brief The time of day: nanoseconds since 1970-01-01 00:00 UTC. Never
 *        sleeps; safe from any CPU. Checks it against the CMOS clock if a
 *        second has passed since the last check and this CPU holds no
 *        spinlock (the clock's lock ranks below many). Goes backwards when
 *        someone sets the time back, or by up to WALLTIME_CHECK_LIMIT_NS
 *        when a check finds it ahead.
 */
uint64_t walltime_ns(void);

/**
 * @brief Compare the time of day with the CMOS clock now (whenever the last
 *        check was), and put it right if it is more than
 *        WALLTIME_CHECK_LIMIT_NS out. Returns how far it was out, in
 *        nanoseconds (positive: ahead). Call holding no spinlock.
 */
int64_t walltime_check(void);

/**
 * @brief For tests: move the time of day by `delta_ns` without telling the
 *        CMOS comparison, as a clock that lost or gained time would have.
 */
void walltime_test_skew(int64_t delta_ns);

/**
 * @brief walltime_ns in whole seconds (what file times are kept in).
 */
uint32_t walltime_seconds(void);

/**
 * @brief Set the time of day (Unix time in nanoseconds). The caller checks
 *        that the user may (root).
 * @return 0, or -EINVAL outside WALLTIME_MIN_SECONDS..WALLTIME_MAX_SECONDS.
 */
int walltime_set(uint64_t unix_ns);

/**
 * @brief Seconds since 1970 of a date and time (proleptic Gregorian
 *        calendar, UTC). Out-of-range fields are not checked: give a real
 *        date.
 */
uint64_t walltime_from_date(const struct rtc_time *date);

/**
 * @brief The date and time of `seconds` since 1970 (the inverse of
 *        walltime_from_date).
 */
void walltime_to_date(uint64_t seconds, struct rtc_time *out);

#endif // WALLTIME_H
