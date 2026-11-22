#include <stdint.h>
#include <tunix/cpu.h>
#include <tunix/percpu.h>
#include <tunix/time.h>
#include <tunix/kprintf.h>
#include <tunix/timer.h>

static uint64_t boot_tsc;
static uint64_t tsc_hz;
static uint64_t boot_realtime_ns;
static int tsc_invariant;
static uint64_t processor_mark[SMP_MAX_CPUS];
static uint64_t processor_skew[SMP_MAX_CPUS];

int time_tsc_is_invariant(void) { return tsc_invariant; }

static int leap_year(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int time_days_in_month(int year, int month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && leap_year(year)) return 29;
    return days[month - 1];
}

uint64_t time_calendar_to_epoch(const struct tunix_rtc_time *value) {
    uint64_t days = 0;
    for (int year = 1970; year < value->year; year++) days += leap_year(year) ? 366U : 365U;
    for (int month = 1; month < value->month; month++)
        days += (uint64_t)time_days_in_month(value->year, month);
    days += (uint64_t)(value->day - 1);
    return days * 86400ULL + (uint64_t)value->hour * 3600ULL + (uint64_t)value->minute * 60ULL +
        (uint64_t)value->second;
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

/* The wall clock is the boot moment plus uptime, moved by settimeofday and
 * adjtimex. A slew (adjtime) moves it gradually by slew_total_ns at 500 ppm,
 * as Linux does. Writers bump realtime_seq around each change so a reader
 * never sees half of one. */
static uint32_t realtime_seq;
static int64_t slew_total_ns;
static uint64_t slew_start_ns;
static uint64_t realtime_generation;

static int64_t slew_applied(uint64_t now) {
    if (!slew_total_ns) return 0;
    uint64_t moved = (now - slew_start_ns) / 2000ULL;
    uint64_t total = slew_total_ns < 0 ? (uint64_t)-slew_total_ns : (uint64_t)slew_total_ns;
    if (moved > total) moved = total;
    return slew_total_ns < 0 ? -(int64_t)moved : (int64_t)moved;
}

uint64_t time_realtime_ns(void) {
    for (;;) {
        uint32_t seq = __atomic_load_n(&realtime_seq, __ATOMIC_ACQUIRE);
        if (seq & 1U) {
            cpu_relax();
            continue;
        }
        uint64_t now = time_uptime_ns();
        uint64_t value = boot_realtime_ns + now + (uint64_t)slew_applied(now);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&realtime_seq, __ATOMIC_RELAXED) == seq) return value;
    }
}

static int realtime_writer;

static void realtime_write_begin(void) {
    while (__atomic_exchange_n(&realtime_writer, 1, __ATOMIC_ACQUIRE)) cpu_relax();
    __atomic_add_fetch(&realtime_seq, 1U, __ATOMIC_ACQ_REL);
}

static void realtime_write_end(void) {
    __atomic_add_fetch(&realtime_seq, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&realtime_writer, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&realtime_generation, 1U, __ATOMIC_RELEASE);
    // Realtime timers armed against the old clock look again at once.
    timer_note_deadline(time_uptime_ns() + 1U);
}

// Folds a slew in progress into the boot moment; called inside a write.
static void slew_fold(uint64_t now) {
    int64_t applied = slew_applied(now);
    boot_realtime_ns += (uint64_t)applied;
    slew_total_ns -= applied;
    slew_start_ns = now;
}

void time_set_realtime_ns(uint64_t value) {
    realtime_write_begin();
    uint64_t now = time_uptime_ns();
    slew_total_ns = 0;
    boot_realtime_ns = value - now;
    realtime_write_end();
}

void time_step_realtime_ns(int64_t delta) {
    realtime_write_begin();
    uint64_t now = time_uptime_ns();
    slew_fold(now);
    boot_realtime_ns += (uint64_t)delta;
    realtime_write_end();
}

int64_t time_slew_realtime_ns(int64_t delta, int set) {
    realtime_write_begin();
    uint64_t now = time_uptime_ns();
    slew_fold(now);
    int64_t left = slew_total_ns;
    if (set) slew_total_ns = delta;
    realtime_write_end();
    return left;
}

uint64_t time_realtime_generation(void) {
    return __atomic_load_n(&realtime_generation, __ATOMIC_ACQUIRE);
}

uint64_t time_epoch_seconds(void) { return time_realtime_ns() / 1000000000ULL; }

uint64_t time_tsc_frequency(void) { return tsc_hz; }

void time_mark_processor(unsigned index) {
    if (index < SMP_MAX_CPUS) processor_mark[index] = time_uptime_ns();
}

void time_check_processor(unsigned index, uint64_t before, uint64_t after) {
    if (index >= SMP_MAX_CPUS) return;
    uint64_t mark = processor_mark[index];
    if (mark >= before && mark <= after) {
        processor_skew[index] = 0;
        return;
    }
    uint64_t skew = mark < before ? before - mark : mark - after;
    processor_skew[index] = skew;
    kprintf("TIME: cpu %u clock disagrees by at least %u ms\n", index,
            (unsigned)(skew / 1000000ULL));
}

uint64_t time_processor_skew(unsigned index) {
    return index < SMP_MAX_CPUS ? processor_skew[index] : 0;
}
