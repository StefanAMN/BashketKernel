/**
 * @file rtc.h
 * @brief CMOS Real-Time Clock (RTC) Hardware Driver Interface
 */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>

struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

/**
 * @brief Initialize RTC CMOS hardware driver.
 */
void rtc_init(void);

/**
 * @brief Read current real-world timestamp from CMOS RTC registers.
 * @param out_time Pointer to output rtc_time structure.
 */
void rtc_read_time(struct rtc_time *out_time);

#endif // RTC_H
