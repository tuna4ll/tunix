#ifndef TUNIX_POSIX_TIMER_H
#define TUNIX_POSIX_TIMER_H

#include <stdint.h>

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define CLOCK_REALTIME_ALARM     8
#define CLOCK_BOOTTIME_ALARM     9
#define CLOCK_TAI                11

#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD_ID 4

#define TIMER_ABSTIME 1

struct posix_timer;

struct posix_timer_event {
    uint64_t value;
    int32_t signo;
    int32_t notify;
    int32_t thread;
};

struct posix_timer_spec {
    uint64_t interval_ns;
    uint64_t value_ns;
};

int posix_clock_read(int clock, uint64_t *ns);
int posix_clock_resolution(int clock, uint64_t *ns);

int posix_timer_create(int clock, const struct posix_timer_event *event, int32_t *id);
int posix_timer_settime(int32_t id, int flags, const struct posix_timer_spec *value,
                        struct posix_timer_spec *old);
int posix_timer_gettime(int32_t id, struct posix_timer_spec *value);
int posix_timer_getoverrun(int32_t id);
int posix_timer_delete(int32_t id);
void posix_timers_release(uint64_t tgid);

uint64_t posix_timers_expire(uint64_t now);
void posix_timer_dequeued(struct posix_timer *timer);
int32_t posix_timer_overrun(struct posix_timer *timer);

#endif
