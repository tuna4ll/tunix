#include <stddef.h>
#include <stdint.h>
#include "include/heap.h"
#include "include/time.h"
#include "include/timerfd.h"

#define EAGAIN 11
#define EINVAL 22
#define TFD_TIMER_ABSTIME 1
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7

struct timerfd_context {
    int clock_id;
    uint64_t interval_ns;
    uint64_t next_expiration_ns;
    uint64_t pending_expirations;
};

static uint64_t clock_now(int clock_id) {
    return clock_id == CLOCK_REALTIME ? time_realtime_ns() : time_uptime_ns();
}

static int timespec_to_ns(int64_t sec, int64_t nsec, uint64_t *out) {
    if (!out || sec < 0 || nsec < 0 || nsec >= 1000000000LL) return -EINVAL;
    if ((uint64_t)sec > UINT64_MAX / 1000000000ULL) return -EINVAL;
    uint64_t value = (uint64_t)sec * 1000000000ULL;
    if (UINT64_MAX - value < (uint64_t)nsec) return -EINVAL;
    *out = value + (uint64_t)nsec;
    return 0;
}

static void ns_to_timespec(uint64_t value, int64_t *sec, int64_t *nsec) {
    *sec = (int64_t)(value / 1000000000ULL);
    *nsec = (int64_t)(value % 1000000000ULL);
}

static void refresh(struct timerfd_context *context) {
    if (!context || !context->next_expiration_ns) return;
    uint64_t now = clock_now(context->clock_id);
    if (now < context->next_expiration_ns) return;
    uint64_t expirations = 1;
    if (context->interval_ns) {
        expirations += (now - context->next_expiration_ns) / context->interval_ns;
        uint64_t advance;
        if (expirations > UINT64_MAX / context->interval_ns)
            advance = UINT64_MAX;
        else
            advance = expirations * context->interval_ns;
        if (UINT64_MAX - context->next_expiration_ns < advance)
            context->next_expiration_ns = UINT64_MAX;
        else
            context->next_expiration_ns += advance;
    } else {
        context->next_expiration_ns = 0;
    }
    if (UINT64_MAX - context->pending_expirations < expirations)
        context->pending_expirations = UINT64_MAX;
    else
        context->pending_expirations += expirations;
}

struct timerfd_context *timerfd_create(int clock_id) {
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC &&
        clock_id != CLOCK_BOOTTIME) return NULL;
    struct timerfd_context *context = kmalloc(sizeof(*context));
    if (!context) return NULL;
    context->clock_id = clock_id;
    context->interval_ns = 0;
    context->next_expiration_ns = 0;
    context->pending_expirations = 0;
    return context;
}

void timerfd_destroy(struct timerfd_context *context) {
    if (context) kfree(context);
}

int timerfd_gettime(struct timerfd_context *context,
                    struct tunix_itimerspec *value) {
    if (!context || !value) return -EINVAL;
    refresh(context);
    uint64_t remaining = 0;
    if (context->next_expiration_ns) {
        uint64_t now = clock_now(context->clock_id);
        if (context->next_expiration_ns > now)
            remaining = context->next_expiration_ns - now;
    }
    ns_to_timespec(context->interval_ns, &value->interval_sec,
                   &value->interval_nsec);
    ns_to_timespec(remaining, &value->value_sec, &value->value_nsec);
    return 0;
}

int timerfd_settime(struct timerfd_context *context, int flags,
                    const struct tunix_itimerspec *new_value,
                    struct tunix_itimerspec *old_value) {
    if (!context || !new_value || (flags & ~TFD_TIMER_ABSTIME)) return -EINVAL;
    if (old_value && timerfd_gettime(context, old_value) != 0) return -EINVAL;
    uint64_t interval;
    uint64_t initial;
    if (timespec_to_ns(new_value->interval_sec, new_value->interval_nsec,
                       &interval) != 0 ||
        timespec_to_ns(new_value->value_sec, new_value->value_nsec,
                       &initial) != 0) return -EINVAL;
    context->interval_ns = interval;
    context->pending_expirations = 0;
    if (!initial) {
        context->next_expiration_ns = 0;
    } else if (flags & TFD_TIMER_ABSTIME) {
        context->next_expiration_ns = initial;
    } else {
        uint64_t now = clock_now(context->clock_id);
        context->next_expiration_ns = UINT64_MAX - now < initial ? UINT64_MAX : now + initial;
    }
    return 0;
}

int64_t timerfd_read(struct timerfd_context *context, size_t size, void *buffer) {
    if (!context || !buffer || size != sizeof(uint64_t)) return -EINVAL;
    refresh(context);
    if (!context->pending_expirations) return -EAGAIN;
    *(uint64_t *)buffer = context->pending_expirations;
    context->pending_expirations = 0;
    return (int64_t)sizeof(uint64_t);
}

int timerfd_read_ready(struct timerfd_context *context) {
    refresh(context);
    return context && context->pending_expirations != 0;
}
