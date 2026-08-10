#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H

#include "types.h"

typedef struct {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
  int weekday;
} rtc_datetime_t;

uint32_t rtc_get_timestamp(void);
int rtc_get_datetime(rtc_datetime_t *dt);

#endif
