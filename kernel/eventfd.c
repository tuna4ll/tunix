#include <stddef.h>
#include <stdint.h>
#include "include/eventfd.h"
#include "include/heap.h"

#define EAGAIN 11
#define EINVAL 22

struct eventfd_context {
    uint64_t counter;
    int semaphore;
};

struct eventfd_context *eventfd_create(uint64_t initial_value, int semaphore) {
    struct eventfd_context *context = kmalloc(sizeof(*context));
    if (!context) return NULL;
    context->counter = initial_value;
    context->semaphore = semaphore != 0;
    return context;
}

void eventfd_destroy(struct eventfd_context *context) {
    if (context) kfree(context);
}

int64_t eventfd_read(struct eventfd_context *context, size_t size, void *buffer) {
    if (!context || !buffer || size != sizeof(uint64_t)) return -EINVAL;
    if (context->counter == 0) return -EAGAIN;
    uint64_t value;
    if (context->semaphore) {
        value = 1;
        context->counter--;
    } else {
        value = context->counter;
        context->counter = 0;
    }
    *(uint64_t *)buffer = value;
    return (int64_t)sizeof(value);
}

int64_t eventfd_write(struct eventfd_context *context, size_t size, const void *buffer) {
    if (!context || !buffer || size != sizeof(uint64_t)) return -EINVAL;
    uint64_t value = *(const uint64_t *)buffer;
    if (value == UINT64_MAX) return -EINVAL;
    if (UINT64_MAX - 1U - context->counter < value) return -EAGAIN;
    context->counter += value;
    return (int64_t)sizeof(value);
}

int eventfd_read_ready(struct eventfd_context *context) {
    return context && context->counter != 0;
}

int eventfd_write_ready(struct eventfd_context *context) {
    return context && context->counter < UINT64_MAX - 1U;
}
