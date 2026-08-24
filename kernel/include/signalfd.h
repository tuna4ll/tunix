#ifndef TUNIX_SIGNALFD_H
#define TUNIX_SIGNALFD_H

#include <stddef.h>
#include <stdint.h>

/*
 * signalfd(2): signal delivery turned into a readable descriptor.
 *
 * This exists for event-loop programs that refuse to do work in a signal
 * handler. Weston is the immediate consumer -- wl_event_loop_add_signal()
 * blocks a signal, opens a signalfd for it and folds the descriptor into the
 * same epoll set as every other source, so SIGINT and SIGCHLD arrive as
 * ordinary loop events.
 *
 * Tunix tracks pending signals as a bitmask rather than a queue of siginfo, so
 * a read reports the signal number and leaves the descriptive fields zero,
 * which is all the fd-based consumers look at.
 */

struct signalfd_context;

struct signalfd_context *signalfd_create(uint64_t mask);
void signalfd_destroy(struct signalfd_context *context);
void signalfd_set_mask(struct signalfd_context *context, uint64_t mask);

/* Consumes as many pending signals as fit in `size`. -EAGAIN when none of the
   descriptor's signals are pending. */
int64_t signalfd_read(struct signalfd_context *context, size_t size, void *buffer);
int signalfd_read_ready(struct signalfd_context *context);

#endif
