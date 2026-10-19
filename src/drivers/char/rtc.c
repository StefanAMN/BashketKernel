/**
 * @file rtc.c
 * @brief CMOS real-time clock (see rtc.h)
 *
 * The clock's registers are reached through an index/data pair of I/O
 * ports: write the register number to 0x70, read its value at 0x71. Writing
 * the index with bit 7 set also keeps NMIs masked, as firmware expects.
 * Register numbers and formats: the MC146818 datasheet, as kept by every PC
 * since the AT (OSDev Wiki, "CMOS").
 */

#include "rtc.h"
#include "io.h"
#include "spinlock.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71
#define NMI_OFF      0x80

#define REG_SECONDS  0x00
#define REG_MINUTES  0x02
#define REG_HOURS    0x04
#define REG_DAY      0x07
#define REG_MONTH    0x08
#define REG_YEAR     0x09
#define REG_STATUS_A 0x0A   // Bit 7: an update is in progress
#define REG_STATUS_B 0x0B   // Bit 1: 24-hour mode; bit 2: binary, not BCD

// The index and the data port are one register's worth of state: two CPUs
// using them at once read each other's registers
static spinlock_t rtc_lock = SPINLOCK_RANKED("rtc", LOCK_RANK_DEVICE);

// One clock register. Caller holds rtc_lock.
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, NMI_OFF | reg);
    return inb(CMOS_DATA);
}

// The time registers in the order rtc_decode takes them. Caller holds rtc_lock.
static void read_registers(uint8_t raw[6]) {
    raw[0] = cmos_read(REG_SECONDS);
    raw[1] = cmos_read(REG_MINUTES);
    raw[2] = cmos_read(REG_HOURS);
    raw[3] = cmos_read(REG_DAY);
    raw[4] = cmos_read(REG_MONTH);
    raw[5] = cmos_read(REG_YEAR);
}

// A BCD byte (two decimal digits, four bits each) as a number
static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

void rtc_decode(const uint8_t raw[6], uint8_t status_b, struct rtc_time *out) {
    bool binary = status_b & 0x04, hours_24 = status_b & 0x02;
    uint8_t v[6];
    for (int i = 0; i < 6; i++) v[i] = binary ? raw[i] : from_bcd(raw[i]);

    // In 12-hour mode bit 7 of the hour means PM (and is not a digit), and
    // the hours run 12, 1, ..., 11: 12 AM is midnight, 12 PM noon
    uint8_t hour = raw[2];
    if (!hours_24) {
        bool pm = hour & 0x80;
        hour = binary ? (uint8_t)(hour & 0x7F) : from_bcd(hour & 0x7F);
        if (hour == 12) hour = 0;
        if (pm) hour = (uint8_t)(hour + 12);
    } else {
        hour = v[2];
    }

    out->second = v[0];
    out->minute = v[1];
    out->hour = hour;
    out->day = v[3];
    out->month = v[4];
    out->year = (uint16_t)(2000 + v[5]);
}

void rtc_read_time(struct rtc_time *out_time) {
    if (!out_time) return;
    uint8_t raw[6], again[6], status_b;

    // The clock updates its registers once a second, one by one. Wait for
    // an update to finish, read everything, and read again until two reads
    // agree: one may have straddled the next update.
    uint64_t flags = spinlock_acquire_irqsave(&rtc_lock);
    for (int i = 0; i < 100000 && (cmos_read(REG_STATUS_A) & 0x80); i++) asm volatile("pause");
    read_registers(raw);
    for (int tries = 0; tries < 8; tries++) {
        for (int i = 0; i < 100000 && (cmos_read(REG_STATUS_A) & 0x80); i++) asm volatile("pause");
        read_registers(again);
        bool same = true;
        for (int i = 0; i < 6; i++) same = same && raw[i] == again[i];
        if (same) break;
        for (int i = 0; i < 6; i++) raw[i] = again[i];
    }
    status_b = cmos_read(REG_STATUS_B);
    spinlock_release_irqrestore(&rtc_lock, flags);

    rtc_decode(raw, status_b, out_time);
}
