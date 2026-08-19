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
 * Times are UTC, as nanoseconds or seconds since 1970-01-01 (Unix time). No
 * time zones, no leap seconds.
 */

#ifndef WALLTIME_H
#define WALLTIME_H

#include <stdint.h>
#include <stdbool.h>
#include "rtc.h"

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
 *        sleeps; safe from any CPU. Goes backwards only when someone sets
 *        the time back.
 */
uint64_t walltime_ns(void);

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
