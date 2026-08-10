#include "drivers/rtc.h"
#include "arch/arch.h"

#if defined(ARCH_X86_64) || defined(ARCH_X86)

#define CMOS_INDEX 0x70
#define CMOS_DATA 0x71
#define CMOS_SECONDS 0x00
#define CMOS_MINUTES 0x02
#define CMOS_HOURS 0x04
#define CMOS_WEEKDAY 0x06
#define CMOS_DAY 0x07
#define CMOS_MONTH 0x08
#define CMOS_YEAR 0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_UIP 0x80
#define CMOS_24_HOUR 0x02
#define CMOS_BINARY 0x04

static uint8_t rtc_cmos_read(uint8_t reg) {
  outb(CMOS_INDEX, reg);
  io_wait();
  return inb(CMOS_DATA);
}

static int rtc_update_in_progress(void) {
  return (rtc_cmos_read(CMOS_STATUS_A) & CMOS_UIP) != 0;
}

static uint8_t rtc_bcd_to_bin(uint8_t value) {
  return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

static int rtc_read_cmos_datetime(rtc_datetime_t *dt) {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t weekday;
  uint8_t day;
  uint8_t month;
  uint8_t year;
  uint8_t last_second;
  uint8_t last_minute;
  uint8_t last_hour;
  uint8_t last_weekday;
  uint8_t last_day;
  uint8_t last_month;
  uint8_t last_year;
  uint8_t status_b;
  unsigned long flags;

  if (!dt)
    return -1;

  for (int tries = 0; tries < 100000 && rtc_update_in_progress(); tries++)
    ;

  flags = arch_irq_save();
  second = rtc_cmos_read(CMOS_SECONDS);
  minute = rtc_cmos_read(CMOS_MINUTES);
  hour = rtc_cmos_read(CMOS_HOURS);
  weekday = rtc_cmos_read(CMOS_WEEKDAY);
  day = rtc_cmos_read(CMOS_DAY);
  month = rtc_cmos_read(CMOS_MONTH);
  year = rtc_cmos_read(CMOS_YEAR);

  do {
    last_second = second;
    last_minute = minute;
    last_hour = hour;
    last_weekday = weekday;
    last_day = day;
    last_month = month;
    last_year = year;

    for (int tries = 0; tries < 100000 && rtc_update_in_progress(); tries++)
      ;

    second = rtc_cmos_read(CMOS_SECONDS);
    minute = rtc_cmos_read(CMOS_MINUTES);
    hour = rtc_cmos_read(CMOS_HOURS);
    weekday = rtc_cmos_read(CMOS_WEEKDAY);
    day = rtc_cmos_read(CMOS_DAY);
    month = rtc_cmos_read(CMOS_MONTH);
    year = rtc_cmos_read(CMOS_YEAR);
  } while (second != last_second || minute != last_minute ||
           hour != last_hour || weekday != last_weekday || day != last_day ||
           month != last_month || year != last_year);

  status_b = rtc_cmos_read(CMOS_STATUS_B);
  arch_irq_restore(flags);

  if ((status_b & CMOS_BINARY) == 0) {
    second = rtc_bcd_to_bin(second);
    minute = rtc_bcd_to_bin(minute);
    hour = (uint8_t)((hour & 0x80) | rtc_bcd_to_bin(hour & 0x7F));
    weekday = rtc_bcd_to_bin(weekday);
    day = rtc_bcd_to_bin(day);
    month = rtc_bcd_to_bin(month);
    year = rtc_bcd_to_bin(year);
  }

  if ((status_b & CMOS_24_HOUR) == 0 && (hour & 0x80))
    hour = (uint8_t)(((hour & 0x7F) + 12) % 24);

  dt->year = 2000 + year;
  dt->month = month;
  dt->day = day;
  dt->hour = hour;
  dt->minute = minute;
  dt->second = second;
  dt->weekday = weekday;

  if (dt->month < 1 || dt->month > 12 || dt->day < 1 || dt->day > 31 ||
      dt->hour > 23 || dt->minute > 59 || dt->second > 60)
    return -1;
  return 0;
}

#else

static int rtc_read_cmos_datetime(rtc_datetime_t *dt) {
  (void)dt;
  return -1;
}

#endif

static int rtc_is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint32_t rtc_datetime_to_unix(const rtc_datetime_t *dt) {
  static const uint16_t days_before_month[12] = {
      0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  uint64_t days = 0;

  for (int year = 1970; year < dt->year; year++)
    days += rtc_is_leap_year(year) ? 366 : 365;

  days += days_before_month[dt->month - 1];
  if (dt->month > 2 && rtc_is_leap_year(dt->year))
    days++;
  days += (uint64_t)(dt->day - 1);

  return (uint32_t)(days * 86400ULL + (uint64_t)dt->hour * 3600ULL +
                    (uint64_t)dt->minute * 60ULL + (uint64_t)dt->second);
}

int rtc_get_datetime(rtc_datetime_t *dt) {
  return rtc_read_cmos_datetime(dt);
}

uint32_t rtc_get_timestamp(void) {
  rtc_datetime_t dt;

  if (rtc_get_datetime(&dt) != 0)
    return 0;
  return rtc_datetime_to_unix(&dt);
}
