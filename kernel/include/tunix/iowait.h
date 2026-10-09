#ifndef TUNIX_IOWAIT_H
#define TUNIX_IOWAIT_H

#include <stdint.h>

struct lock;

#define IO_TIMEOUT_NS (10ULL * 1000ULL * 1000ULL * 1000ULL)

typedef int (*io_ready_fn)(void *context);

int io_poll(io_ready_fn ready, void *context, uint64_t timeout_ns);
int io_poll_dropping(io_ready_fn ready, void *context, uint64_t timeout_ns, struct lock *held,
                     const void *channel);
void io_poll_tick(void);
void io_nap(struct lock *held);

#endif
