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

int time_days_in_month(int year, int month);
uint64_t time_calendar_to_epoch(const struct tunix_rtc_time *value);
void time_epoch_to_calendar(uint64_t seconds, struct tunix_rtc_time *out);

uint64_t arch_clock_frequency(void);
int arch_clock_invariant(void);
int arch_rtc_read(struct tunix_rtc_time *out);

void time_init(void);
uint64_t time_uptime_ns(void);
uint64_t time_realtime_ns(void);
uint64_t time_epoch_seconds(void);
uint64_t time_tsc_frequency(void);
int time_get_rtc(struct tunix_rtc_time *out);
int time_tsc_is_invariant(void);
void time_mark_processor(unsigned index);
void time_check_processor(unsigned index, uint64_t before, uint64_t after);
uint64_t time_processor_skew(unsigned index);

#endif
