#include <stdint.h>

#include "../../include/cpu.h"
#include "../../include/io.h"
#include "../../include/time.h"

#define PIT_FREQUENCY 1193182ULL
#define PIT_SAMPLE_TICKS 23864U
#define CMOS_ADDRESS 0x70U
#define CMOS_DATA 0x71U
#define CMOS_UPDATE_IN_PROGRESS 0x80U

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

int arch_clock_invariant(void) {
    uint32_t a, b, c, d;
    cpu_cpuid(0x80000000U, 0, &a, &b, &c, &d);
    if (a < 0x80000007U) return 0;
    cpu_cpuid(0x80000007U, 0, &a, &b, &c, &d);
    return (d & (1U << 8)) != 0;
}

static uint64_t frequency_from_cpuid(void) {
    uint32_t a, b, c, d;
    cpu_cpuid(0, 0, &a, &b, &c, &d);
    uint32_t maximum = a;
    if (maximum >= 0x15U) {
        cpu_cpuid(0x15U, 0, &a, &b, &c, &d);
        if (a && b && c) return ((uint64_t)c * b) / a;
    }
    if (maximum >= 0x16U) {
        cpu_cpuid(0x16U, 0, &a, &b, &c, &d);
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

    uint64_t start = cpu_counter_ordered();
    uint64_t limit = start + 1000000000ULL;
    while ((inb(0x61) & 0x20U) == 0) {
        if (cpu_counter_ordered() > limit) {
            outb(0x61, original);
            return 0;
        }
        cpu_relax();
    }
    uint64_t end = cpu_counter_ordered();
    outb(0x61, original);
    if (end <= start) return 0;
    return ((end - start) * PIT_FREQUENCY) / PIT_SAMPLE_TICKS;
}

uint64_t arch_clock_frequency(void) {
    uint64_t hz = frequency_from_cpuid();
    return hz ? hz : frequency_from_pit();
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
        raw.day > (uint8_t)time_days_in_month(year, raw.month) || raw.hour > 23U ||
        raw.minute > 59U || raw.second > 59U) return -1;

    out->year = year;
    out->month = raw.month;
    out->day = raw.day;
    out->hour = raw.hour;
    out->minute = raw.minute;
    out->second = raw.second;
    time_epoch_to_calendar(time_calendar_to_epoch(out), out);
    return 0;
}

int arch_rtc_read(struct tunix_rtc_time *out) {
    struct rtc_snapshot first;
    struct rtc_snapshot second;
    for (unsigned attempt = 0; attempt < 100000U; attempt++) {
        unsigned timeout = 1000000U;
        while (timeout && rtc_updating()) {
            timeout--;
            cpu_relax();
        }
        if (!timeout) break;
        rtc_read_once(&first);
        timeout = 1000000U;
        while (timeout && rtc_updating()) {
            timeout--;
            cpu_relax();
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
