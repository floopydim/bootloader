#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/*
 * CMOS RTC (ports 0x70/0x71).
 *
 * Reads wall time with update-in-progress protection, handling both
 * binary and BCD mode, and the century register (declared in FADT).
 */

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;      /* full year incl. century, e.g. 2026 */
} RTC_TIME;

/* Returns 1 on success (stable read), 0 if the RTC did not respond. */
int rtc_get_time(RTC_TIME *out);

/* Force the century register (0x32 default) */
void rtc_set_century_reg(uint8_t reg);

#endif
