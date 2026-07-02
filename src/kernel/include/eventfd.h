#ifndef TUNIX_EVENTFD_H
#define TUNIX_EVENTFD_H

#include <stddef.h>
#include <stdint.h>

struct eventfd_context;

struct eventfd_context *eventfd_create(uint64_t initial_value, int semaphore);
void eventfd_destroy(struct eventfd_context *context);
int64_t eventfd_read(struct eventfd_context *context, size_t size, void *buffer);
int64_t eventfd_write(struct eventfd_context *context, size_t size, const void *buffer);
int eventfd_read_ready(struct eventfd_context *context);
int eventfd_write_ready(struct eventfd_context *context);

#endif
