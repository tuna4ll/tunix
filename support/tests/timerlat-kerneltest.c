typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_WRITE 1
#define SYS_NANOSLEEP 35
#define SYS_FUTEX 202
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231
#define SYS_EPOLL_WAIT 232
#define SYS_EPOLL_CTL 233
#define SYS_TIMERFD_CREATE 283
#define SYS_TIMERFD_SETTIME 286
#define SYS_EPOLL_CREATE1 291
#define SYS_READ 0

#define CLOCK_MONOTONIC 1
#define FUTEX_WAIT_PRIVATE 128
#define EPOLL_CTL_ADD 1
#define EPOLLIN 1U
#define THREAD_FLAGS 0x10F00UL
#define SAMPLES 200U
#define FRAMES 120U
#define FRAME_NS 16666667UL
#define SPINNERS 4U

#if defined(__x86_64__)
struct epoll_event {
    unsigned events;
    u64 data;
} __attribute__((packed));
#else
struct epoll_event {
    unsigned events;
    u64 data;
};
#endif

struct timespec {
    s64 sec;
    s64 nsec;
};

struct itimerspec {
    struct timespec interval;
    struct timespec value;
};

static char stacks[SPINNERS][65536] __attribute__((aligned(16)));
static volatile unsigned spin_stop;
static volatile unsigned spinning[SPINNERS];
static volatile unsigned next_spinner;
static u64 samples[SAMPLES > FRAMES ? SAMPLES : FRAMES];
static unsigned failures;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)text, (s64)length);
}

static void put_number(u64 value) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    char out[24];
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
    put(out);
}

static u64 now_ns(void) {
    struct timespec value;
    (void)syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.sec * 1000000000UL + (u64)value.nsec;
}

static void sort(u64 *values, unsigned count) {
    for (unsigned i = 1; i < count; i++) {
        u64 key = values[i];
        unsigned j = i;
        while (j && values[j - 1] > key) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = key;
    }
}

static void report(const char *name, unsigned count, u64 limit_us) {
    sort(samples, count);
    u64 median = samples[count / 2] / 1000U;
    u64 p99 = samples[(count * 99U) / 100U] / 1000U;
    u64 worst = samples[count - 1] / 1000U;
    put("TIMERLAT ");
    put(name);
    put(" median_us=");
    put_number(median);
    put(" p99_us=");
    put_number(p99);
    put(" max_us=");
    put_number(worst);
    if (limit_us && median > limit_us) {
        put(" SLOW");
        failures++;
    }
    put("\n");
}

static void sleep_ns(u64 duration) {
    struct timespec request = {(s64)(duration / 1000000000UL), (s64)(duration % 1000000000UL)};
    (void)syscall2(SYS_NANOSLEEP, (s64)&request, 0);
}

static void measure_nanosleep(const char *name, u64 duration, u64 limit_us) {
    for (unsigned index = 0; index < SAMPLES; index++) {
        u64 start = now_ns();
        sleep_ns(duration);
        u64 elapsed = now_ns() - start;
        samples[index] = elapsed > duration ? elapsed - duration : 0;
    }
    report(name, SAMPLES, limit_us);
}

static void measure_epoll_timeout(void) {
    int epoll = (int)syscall1(SYS_EPOLL_CREATE1, 0);
    struct epoll_event event;
    for (unsigned index = 0; index < SAMPLES; index++) {
        u64 start = now_ns();
        (void)syscall4(SYS_EPOLL_WAIT, epoll, (s64)&event, 1, 2);
        u64 elapsed = now_ns() - start;
        samples[index] = elapsed > 2000000UL ? elapsed - 2000000UL : 0;
    }
    report("epoll_wait_2ms", SAMPLES, 1000);
}

static void measure_futex_timeout(void) {
    static volatile unsigned word;
    struct timespec timeout = {0, 1000000};
    for (unsigned index = 0; index < SAMPLES; index++) {
        u64 start = now_ns();
        (void)syscall4(SYS_FUTEX, (s64)&word, FUTEX_WAIT_PRIVATE, 0, (s64)&timeout);
        u64 elapsed = now_ns() - start;
        samples[index] = elapsed > 1000000UL ? elapsed - 1000000UL : 0;
    }
    report("futex_1ms", SAMPLES, 500);
}

static void measure_frame_clock(const char *name, u64 limit_us) {
    int timer = (int)syscall2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0);
    int epoll = (int)syscall1(SYS_EPOLL_CREATE1, 0);
    if (timer < 0 || epoll < 0) {
        put("TIMERLAT no timerfd\n");
        failures++;
        return;
    }
    struct epoll_event event = {EPOLLIN, 0};
    (void)syscall4(SYS_EPOLL_CTL, epoll, EPOLL_CTL_ADD, timer, (s64)&event);
    u64 first = now_ns() + FRAME_NS;
    struct itimerspec setting = {{0, (s64)FRAME_NS}, {0, (s64)FRAME_NS}};
    (void)syscall4(SYS_TIMERFD_SETTIME, timer, 0, (s64)&setting, 0);
    u64 expected = first;
    for (unsigned index = 0; index < FRAMES; index++) {
        (void)syscall4(SYS_EPOLL_WAIT, epoll, (s64)&event, 1, -1);
        u64 woke = now_ns();
        u64 count = 0;
        (void)syscall3(SYS_READ, timer, (s64)&count, sizeof(count));
        if (!count) count = 1;
        expected += (count - 1) * FRAME_NS;
        samples[index] = woke > expected ? woke - expected : 0;
        expected += FRAME_NS;
    }
    report(name, FRAMES, limit_us);
}

static unsigned spinners_running(void) {
    unsigned count = 0;
    for (unsigned index = 0; index < SPINNERS; index++) count += spinning[index];
    return count;
}

static void spinner(void) {
    unsigned self = next_spinner;
    spinning[self] = 1;
    while (!spin_stop) __asm__ volatile("" ::: "memory");
    spinning[self] = 0;
}

static void start_spinners(void) {
    for (unsigned index = 0; index < SPINNERS; index++) {
        char *top = stacks[index] + sizeof(stacks[index]) - 16;
        *(void **)top = (void *)spinner;
        next_spinner = index;
        if (spawn_thread(THREAD_FLAGS, top) < 0) {
            put("TIMERLAT cannot start a spinner\n");
            failures++;
            return;
        }
        while (!spinning[index]) sleep_ns(1000000UL);
    }
}

static void stop_spinners(void) {
    spin_stop = 1;
    while (spinners_running()) sleep_ns(1000000UL);
}

void run(void) {
    measure_nanosleep("nanosleep_1ms", 1000000UL, 500);
    measure_nanosleep("nanosleep_100us", 100000UL, 500);
    measure_epoll_timeout();
    measure_futex_timeout();
    measure_frame_clock("timerfd_60hz_lateness", 1000);
    start_spinners();
    measure_nanosleep("loaded_nanosleep_1ms", 1000000UL, 0);
    measure_frame_clock("loaded_timerfd_60hz_lateness", 0);
    stop_spinners();
    put(failures ? "TIMERLAT FAIL\n" : "TIMERLAT PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
}

TUNIX_START(run)
