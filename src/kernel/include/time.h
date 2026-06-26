#ifndef TUNIX_TIME_H
#define TUNIX_TIME_H

#include <stdint.h>

struct tunix_rtc_time {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int weekday;
    int yearday;
};

void time_init(void);
uint64_t time_uptime_ns(void);
uint64_t time_realtime_ns(void);
uint64_t time_epoch_seconds(void);
uint64_t time_tsc_frequency(void);
int time_get_rtc(struct tunix_rtc_time *out);

#endif
