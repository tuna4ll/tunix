#ifndef TUNIX_TIMERFD_H
#define TUNIX_TIMERFD_H

#include <stddef.h>
#include <stdint.h>

struct timerfd_context;

struct tunix_itimerspec {
    int64_t interval_sec;
    int64_t interval_nsec;
    int64_t value_sec;
    int64_t value_nsec;
};

struct timerfd_context *timerfd_create(int clock_id);
void timerfd_destroy(struct timerfd_context *context);
int timerfd_settime(struct timerfd_context *context, int flags,
                    const struct tunix_itimerspec *new_value,
                    struct tunix_itimerspec *old_value);
int timerfd_gettime(struct timerfd_context *context,
                    struct tunix_itimerspec *value);
int64_t timerfd_read(struct timerfd_context *context, size_t size, void *buffer);
int timerfd_read_ready(struct timerfd_context *context);

#endif
