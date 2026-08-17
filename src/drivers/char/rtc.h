/**
 * @file rtc.h
 * @brief CMOS Real-Time Clock (RTC) Hardware Driver Interface
 */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stdbool.h>

struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

/**
 * @brief Read the date and time from the CMOS clock (as the firmware keeps
 *        it: no time zone is applied; the century is taken to be 2000).
 *        Safe from any CPU; spins for at most the length of one clock
 *        update. Nothing is checked: a clock that was never set gives
 *        whatever it holds.
 */
void rtc_read_time(struct rtc_time *out_time);

/**
 * @brief Turn the clock's six time registers (seconds, minutes, hours, day,
 *        month, year, as read) into a date, as status register B says they
 *        are kept: BCD or binary, 12- or 24-hour. (Separate for the tests.)
 */
void rtc_decode(const uint8_t raw[6], uint8_t status_b, struct rtc_time *out);

#endif // RTC_H
