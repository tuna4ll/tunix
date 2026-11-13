#ifndef TUNIX_SIGNALFD_H
#define TUNIX_SIGNALFD_H

#include <stddef.h>
#include <stdint.h>

struct signalfd_context;

struct signalfd_context *signalfd_create(uint64_t mask);
void signalfd_destroy(struct signalfd_context *context);
void signalfd_set_mask(struct signalfd_context *context, uint64_t mask);

int64_t signalfd_read(struct signalfd_context *context, size_t size, void *buffer);
int signalfd_read_ready(struct signalfd_context *context);

#endif
