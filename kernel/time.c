#include <stdint.h>
#include "include/cpu.h"
#include "include/percpu.h"
#include "include/time.h"

extern void panic(const char *message) __attribute__((noreturn));
extern void kprintf(const char *fmt, ...);

static uint64_t boot_tsc;
static uint64_t tsc_hz;
static uint64_t boot_realtime_ns;
static int tsc_invariant;
static uint64_t processor_mark[SMP_MAX_CPUS];
static uint64_t processor_skew[SMP_MAX_CPUS];

int time_tsc_is_invariant(void) { return tsc_invariant; }

static int leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int time_days_in_month(int year, int month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && leap_year(year)) return 29;
    return days[month - 1];
}

uint64_t time_calendar_to_epoch(const struct tunix_rtc_time *value) {
    uint64_t days = 0;
    for (int year = 1970; year < value->year; year++) days += leap_year(year) ? 366U : 365U;
    for (int month = 1; month < value->month; month++) days += (uint64_t)time_days_in_month(value->year, month);
    days += (uint64_t)(value->day - 1);
    return days * 86400ULL + (uint64_t)value->hour * 3600ULL +
           (uint64_t)value->minute * 60ULL + (uint64_t)value->second;
}

void time_epoch_to_calendar(uint64_t seconds, struct tunix_rtc_time *out) {
    uint64_t days = seconds / 86400ULL;
    uint64_t rest = seconds % 86400ULL;
    out->hour = (int)(rest / 3600ULL);
    out->minute = (int)((rest % 3600ULL) / 60ULL);
    out->second = (int)(rest % 60ULL);
    out->weekday = (int)((days + 4U) % 7U);

    int year = 1970;
    for (;;) {
        uint64_t length = leap_year(year) ? 366U : 365U;
        if (days < length) break;
        days -= length;
        year++;
    }
    out->year = year;
    out->yearday = (int)days;

    int month = 1;
    while (days >= (uint64_t)time_days_in_month(year, month)) {
        days -= (uint64_t)time_days_in_month(year, month);
        month++;
    }
    out->month = month;
    out->day = (int)days + 1;
}

int time_get_rtc(struct tunix_rtc_time *out) {
    if (!out) return -1;
    return arch_rtc_read(out);
}

void time_init(void) {
    tsc_hz = arch_clock_frequency();
    if (tsc_hz < 1000000ULL) panic("unable to calibrate TSC");
    boot_tsc = cpu_counter_ordered();
    tsc_invariant = arch_clock_invariant();

    struct tunix_rtc_time rtc;
    if (time_get_rtc(&rtc) != 0) panic("unable to read CMOS RTC");
    boot_realtime_ns = time_calendar_to_epoch(&rtc) * 1000000000ULL;
}

uint64_t time_uptime_ns(void) {
    uint64_t raw = cpu_counter_ordered();
    if (raw < boot_tsc) return 0;
    uint64_t delta = raw - boot_tsc;
    uint64_t seconds = delta / tsc_hz;
    uint64_t remainder = delta % tsc_hz;
    return seconds * 1000000000ULL + (remainder * 1000000000ULL) / tsc_hz;
}

uint64_t time_realtime_ns(void) {
    return boot_realtime_ns + time_uptime_ns();
}

uint64_t time_epoch_seconds(void) {
    return time_realtime_ns() / 1000000000ULL;
}

uint64_t time_tsc_frequency(void) {
    return tsc_hz;
}

void time_mark_processor(unsigned index) {
    if (index < SMP_MAX_CPUS) processor_mark[index] = time_uptime_ns();
}

void time_check_processor(unsigned index, uint64_t before, uint64_t after) {
    if (index >= SMP_MAX_CPUS) return;
    uint64_t mark = processor_mark[index];
    if (mark >= before && mark <= after) { processor_skew[index] = 0; return; }
    uint64_t skew = mark < before ? before - mark : mark - after;
    processor_skew[index] = skew;
    kprintf("TIME: cpu %u clock disagrees by at least %u ms\n", index,
            (unsigned)(skew / 1000000ULL));
}

uint64_t time_processor_skew(unsigned index) {
    return index < SMP_MAX_CPUS ? processor_skew[index] : 0;
}
