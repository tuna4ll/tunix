#include <stdint.h>
#include "include/io.h"
#include "include/time.h"

#define PIT_FREQUENCY 1193182ULL
#define PIT_SAMPLE_TICKS 23864U
#define CMOS_ADDRESS 0x70U
#define CMOS_DATA 0x71U
#define CMOS_UPDATE_IN_PROGRESS 0x80U

extern void panic(const char *message) __attribute__((noreturn));

struct rtc_snapshot {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    uint8_t status_b;
};

static uint64_t boot_tsc;
static uint64_t tsc_hz;
static uint64_t boot_realtime_ns;

static inline uint64_t read_tsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | low;
}

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static uint64_t frequency_from_cpuid(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t maximum = a;
    if (maximum >= 0x15U) {
        cpuid(0x15U, 0, &a, &b, &c, &d);
        if (a && b && c) return ((uint64_t)c * b) / a;
    }
    if (maximum >= 0x16U) {
        cpuid(0x16U, 0, &a, &b, &c, &d);
        if (a) return (uint64_t)a * 1000000ULL;
    }
    return 0;
}

static uint64_t frequency_from_pit(void) {
    uint8_t original = inb(0x61);
    outb(0x61, original & (uint8_t)~0x01U);
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)(PIT_SAMPLE_TICKS & 0xFFU));
    outb(0x42, (uint8_t)(PIT_SAMPLE_TICKS >> 8));
    outb(0x61, (original & (uint8_t)~0x02U) | 0x01U);

    uint64_t start = read_tsc();
    uint64_t limit = start + 1000000000ULL;
    while ((inb(0x61) & 0x20U) == 0) {
        if (read_tsc() > limit) {
            outb(0x61, original);
            return 0;
        }
        __asm__ volatile("pause");
    }
    uint64_t end = read_tsc();
    outb(0x61, original);
    if (end <= start) return 0;
    return ((end - start) * PIT_FREQUENCY) / PIT_SAMPLE_TICKS;
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, (uint8_t)(0x80U | reg));
    io_wait();
    return inb(CMOS_DATA);
}

static int rtc_updating(void) {
    return (cmos_read(0x0AU) & CMOS_UPDATE_IN_PROGRESS) != 0;
}

static void rtc_read_once(struct rtc_snapshot *value) {
    value->second = cmos_read(0x00U);
    value->minute = cmos_read(0x02U);
    value->hour = cmos_read(0x04U);
    value->day = cmos_read(0x07U);
    value->month = cmos_read(0x08U);
    value->year = cmos_read(0x09U);
    value->century = cmos_read(0x32U);
    value->status_b = cmos_read(0x0BU);
}

static int rtc_equal(const struct rtc_snapshot *a, const struct rtc_snapshot *b) {
    return a->second == b->second && a->minute == b->minute &&
           a->hour == b->hour && a->day == b->day && a->month == b->month &&
           a->year == b->year && a->century == b->century &&
           a->status_b == b->status_b;
}

static uint8_t from_bcd(uint8_t value) {
    return (uint8_t)((value & 0x0FU) + ((value >> 4) * 10U));
}

static int leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && leap_year(year)) return 29;
    return days[month - 1];
}

static int rtc_decode(struct rtc_snapshot raw, struct tunix_rtc_time *out) {
    int pm = (raw.hour & 0x80U) != 0;
    raw.hour &= 0x7FU;
    if (!(raw.status_b & 0x04U)) {
        raw.second = from_bcd(raw.second);
        raw.minute = from_bcd(raw.minute);
        raw.hour = from_bcd(raw.hour);
        raw.day = from_bcd(raw.day);
        raw.month = from_bcd(raw.month);
        raw.year = from_bcd(raw.year);
        raw.century = from_bcd(raw.century);
    }
    if (!(raw.status_b & 0x02U)) {
        if (pm && raw.hour < 12U) raw.hour = (uint8_t)(raw.hour + 12U);
        if (!pm && raw.hour == 12U) raw.hour = 0;
    }

    int century = raw.century >= 19U && raw.century <= 99U ? raw.century : 20;
    int year = century * 100 + raw.year;
    if (year < 1970 || raw.month < 1U || raw.month > 12U || raw.day < 1U ||
        raw.day > (uint8_t)days_in_month(year, raw.month) || raw.hour > 23U ||
        raw.minute > 59U || raw.second > 59U) return -1;

    int yearday = 0;
    for (int month = 1; month < raw.month; month++) yearday += days_in_month(year, month);
    yearday += raw.day - 1;

    uint64_t days = 0;
    for (int current = 1970; current < year; current++) days += leap_year(current) ? 366U : 365U;
    days += (uint64_t)yearday;

    out->year = year;
    out->month = raw.month;
    out->day = raw.day;
    out->hour = raw.hour;
    out->minute = raw.minute;
    out->second = raw.second;
    out->weekday = (int)((days + 4U) % 7U);
    out->yearday = yearday;
    return 0;
}

static uint64_t rtc_to_epoch(const struct tunix_rtc_time *value) {
    uint64_t days = 0;
    for (int year = 1970; year < value->year; year++) days += leap_year(year) ? 366U : 365U;
    for (int month = 1; month < value->month; month++) days += (uint64_t)days_in_month(value->year, month);
    days += (uint64_t)(value->day - 1);
    return days * 86400ULL + (uint64_t)value->hour * 3600ULL +
           (uint64_t)value->minute * 60ULL + (uint64_t)value->second;
}

int time_get_rtc(struct tunix_rtc_time *out) {
    if (!out) return -1;
    struct rtc_snapshot first;
    struct rtc_snapshot second;
    for (unsigned attempt = 0; attempt < 100000U; attempt++) {
        unsigned timeout = 1000000U;
        while (timeout && rtc_updating()) {
            timeout--;
            __asm__ volatile("pause");
        }
        if (!timeout) break;
        rtc_read_once(&first);
        timeout = 1000000U;
        while (timeout && rtc_updating()) {
            timeout--;
            __asm__ volatile("pause");
        }
        if (!timeout) break;
        rtc_read_once(&second);
        if (rtc_equal(&first, &second)) {
            outb(CMOS_ADDRESS, 0x00U);
            return rtc_decode(second, out);
        }
    }
    outb(CMOS_ADDRESS, 0x00U);
    return -1;
}

void time_init(void) {
    tsc_hz = frequency_from_cpuid();
    if (!tsc_hz) tsc_hz = frequency_from_pit();
    if (tsc_hz < 1000000ULL) panic("unable to calibrate TSC");
    boot_tsc = read_tsc();

    struct tunix_rtc_time rtc;
    if (time_get_rtc(&rtc) != 0) panic("unable to read CMOS RTC");
    boot_realtime_ns = rtc_to_epoch(&rtc) * 1000000000ULL;
}

uint64_t time_uptime_ns(void) {
    uint64_t delta = read_tsc() - boot_tsc;
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
