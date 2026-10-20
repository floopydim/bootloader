#include "rtc.h"
#include "io.h"

#define RTC_ADDR_PORT 0x70
#define RTC_DATA_PORT 0x71

#define RTC_STAT_A      0x0A
#define RTC_STAT_B      0x0B
#define RTC_SEC         0x00
#define RTC_MIN         0x02
#define RTC_HOUR        0x04
#define RTC_DAY         0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_CENTURY     0x32

static uint8_t g_century_reg = RTC_CENTURY;
static int g_fallback_bcd = -1;  /* cache B flag interpretation */

static uint8_t rtc_read_reg(uint8_t reg) {
    outb(RTC_ADDR_PORT, reg);
    return inb(RTC_DATA_PORT);
}

static int rtc_is_bcd(void) {
    uint8_t status_b;

    if (g_fallback_bcd >= 0) {
        return g_fallback_bcd;
    }

    status_b = rtc_read_reg(RTC_STAT_B);
    /* Bit 2 = data mode: 1 = binary, 0 = BCD */
    g_fallback_bcd = (status_b & 0x04) ? 0 : 1;
    return g_fallback_bcd;
}

static int rtc_from_bcd(uint8_t value) {
    return (value & 0x0F) + ((value >> 4) * 10);
}

static uint8_t rtc_update_in_progress(void) {
    return rtc_read_reg(RTC_STAT_A) & 0x80;
}

static uint8_t rtc_decode(uint8_t raw) {
    if (!rtc_is_bcd()) {
        return raw;
    }
    if ((raw & 0x0F) <= 9 && ((raw >> 4) & 0x0F) <= 9) {
        return rtc_from_bcd(raw);
    }
    return raw;
}

/*
 * Read the whole time twice while waiting for the UIP flag to drop,
 * retrying until the two consecutive reads agree or attempts run out.
 */
static int rtc_read_stable(RTC_TIME *t) {
    uint32_t attempt;
    uint32_t inner;

    for (attempt = 0; attempt < 16; attempt++) {
        RTC_TIME first;
        RTC_TIME second;

        inner = 0;
        while (rtc_update_in_progress() && inner < 100000U) {
            inner++;
        }

        first.second = rtc_decode(rtc_read_reg(RTC_SEC));
        first.minute = rtc_decode(rtc_read_reg(RTC_MIN));
        first.hour   = rtc_decode(rtc_read_reg(RTC_HOUR));
        first.day    = rtc_decode(rtc_read_reg(RTC_DAY));
        first.month  = rtc_decode(rtc_read_reg(RTC_MONTH));
        first.year   = rtc_decode(rtc_read_reg(RTC_YEAR));
        {
            uint8_t century = rtc_decode(rtc_read_reg(g_century_reg));
            first.year += (uint16_t)(century * 100);
        }

        inner = 0;
        while (rtc_update_in_progress() && inner < 100000U) {
            inner++;
        }

        second.second = rtc_decode(rtc_read_reg(RTC_SEC));
        second.minute = rtc_decode(rtc_read_reg(RTC_MIN));
        second.hour   = rtc_decode(rtc_read_reg(RTC_HOUR));
        second.day    = rtc_decode(rtc_read_reg(RTC_DAY));
        second.month  = rtc_decode(rtc_read_reg(RTC_MONTH));
        second.year   = rtc_decode(rtc_read_reg(RTC_YEAR));
        {
            uint8_t century = rtc_decode(rtc_read_reg(g_century_reg));
            second.year += (uint16_t)(century * 100);
        }

        if (first.second == second.second &&
            first.minute == second.minute &&
            first.hour == second.hour &&
            first.day == second.day &&
            first.month == second.month &&
            first.year == second.year) {
            *t = first;
            return 1;
        }
    }

    return 0;
}

void rtc_set_century_reg(uint8_t reg) {
    g_century_reg = reg;
    g_fallback_bcd = -1;
}

int rtc_get_time(RTC_TIME *out) {
    if (!out) {
        return 0;
    }

    if (!rtc_read_stable(out)) {
        return 0;
    }

    /* Sanity: reject obviously bogus values from a dead CMOS. */
    if (out->month < 1 || out->month > 12 ||
        out->day < 1 || out->day > 31 ||
        (out->year % 4) > 3 ||
        out->hour > 23 || out->minute > 59 || out->second > 61) {
        return 0;
    }

    return 1;
}
