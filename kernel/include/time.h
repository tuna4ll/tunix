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
/* Whether the processor promises a TSC that counts at a constant rate and never
   stops, which every deadline and every slice here assumes. */
int time_tsc_is_invariant(void);
/* This processor's own reading of the clock, taken as it comes up, for the
   processor that started it to check against. */
void time_mark_processor(unsigned index);
/* Say so if that reading did not fall between the two the starter took around
   it, which is the only way from here to see counters that disagree. */
void time_check_processor(unsigned index, uint64_t before, uint64_t after);

#endif
