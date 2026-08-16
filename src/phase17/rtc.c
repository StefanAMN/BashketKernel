/**
 * @file rtc.c
 * @brief Implementation of CMOS Real-Time Clock (RTC) Driver
 */

#include "rtc.h"
#include "io.h"
#include "kprintf.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, (1 << 7) | reg); // Bit 7 disables NMI
    return inb(CMOS_DATA);
}

static int get_update_in_progress_flag(void) {
    outb(CMOS_ADDRESS, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

void rtc_init(void) {
    kprintf("RTC Init: CMOS Real-Time Clock driver ready.\n");
}

void rtc_read_time(struct rtc_time *out_time) {
    if (!out_time) return;

    // Wait until update in progress finishes
    while (get_update_in_progress_flag());

    uint8_t sec   = cmos_read(0x00);
    uint8_t min   = cmos_read(0x02);
    uint8_t hour  = cmos_read(0x04);
    uint8_t day   = cmos_read(0x07);
    uint8_t month = cmos_read(0x08);
    uint8_t year  = cmos_read(0x09);

    uint8_t status_b = cmos_read(0x0B);

    // Convert BCD to Binary if bit 2 of Status Register B is 0
    if (!(status_b & 0x04)) {
        sec   = (sec & 0x0F) + ((sec / 16) * 10);
        min   = (min & 0x0F) + ((min / 16) * 10);
        hour  = ((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80);
        day   = (day & 0x0F) + ((day / 16) * 10);
        month = (month & 0x0F) + ((month / 16) * 10);
        year  = (year & 0x0F) + ((year / 16) * 10);
    }

    // Convert 12h clock format to 24h if necessary
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    out_time->second = sec;
    out_time->minute = min;
    out_time->hour   = hour;
    out_time->day    = day;
    out_time->month  = month;
    out_time->year   = 2000 + year;
}
