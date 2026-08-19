/**
 * @file date.h
 * @brief Days and dates in the proleptic Gregorian calendar
 *
 * The arithmetic between "days since 1970-01-01" and a year, month and day,
 * shared by the kernel (file times, the time of day) and the user library
 * (date, ls). This file is compiled twice, like sha256.c.
 *
 * The algorithms are Howard Hinnant's ("chrono-compatible low-level date
 * algorithms"): they count in eras of 400 years (146097 days), within which
 * the calendar repeats exactly, and start each year in March, so that the
 * leap day falls at its end.
 */

#ifndef LIB_TIME_DATE_H
#define LIB_TIME_DATE_H

#include <stdint.h>

/**
 * @brief Days since 1970-01-01 of a date (negative before it). Month 1-12,
 *        day 1-31; nothing is checked.
 */
int64_t days_from_civil(int64_t year, unsigned month, unsigned day);

/**
 * @brief The date of a day counted from 1970-01-01 (the inverse).
 */
void civil_from_days(int64_t days, unsigned *year, unsigned *month, unsigned *day);

#endif // LIB_TIME_DATE_H
