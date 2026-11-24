#include <stddef.h>
#include <stdint.h>
#include <tunix/eventfd.h>
#include <tunix/cpu.h>
#include <tunix/heap.h>
#include <tunix/lock.h>
#include <tunix/errno.h>

static struct lock eventfd_lock = LOCK_INITIALIZER("eventfd", LOCK_RANK_OBJECT);

static void eventfd_guard_release(int *unused) {
    (void)unused;
    lock_release(&eventfd_lock);
}

#define EVENTFD_LOCKED \
    __attribute__((cleanup(eventfd_guard_release))) int eventfd_guard = \
        (lock_acquire(&eventfd_lock), 0)

struct eventfd_context {
    uint64_t counter;
    int semaphore;
    /* Bumped by every write: mio's waker writes without ever reading, and
     * relies on each write being a new edge-triggered event. */
    uint32_t generation;
    void (*consume)(void *argument);
    void *consume_argument;
    int consuming;
};

struct eventfd_context *eventfd_create(uint64_t initial_value, int semaphore) {
    EVENTFD_LOCKED;
    struct eventfd_context *context = kmalloc(sizeof(*context));
    if (!context) return NULL;
    context->counter = initial_value;
    context->semaphore = semaphore != 0;
    context->generation = 0;
    context->consume = NULL;
    context->consume_argument = NULL;
    context->consuming = 0;
    return context;
}

void eventfd_destroy(struct eventfd_context *context) {
    EVENTFD_LOCKED;
    if (context) kfree(context);
}

int64_t eventfd_read(struct eventfd_context *context, size_t size, void *buffer) {
    EVENTFD_LOCKED;
    if (!context || !buffer || size < sizeof(uint64_t)) return -EINVAL;
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
    void (*consume)(void *argument) = NULL;
    void *argument = NULL;
    {
        EVENTFD_LOCKED;
        if (!context || !buffer || size < sizeof(uint64_t)) return -EINVAL;
        uint64_t value = *(const uint64_t *)buffer;
        if (value == UINT64_MAX) return -EINVAL;
        context->generation++;
        if (context->consume) {
            if (!value) return (int64_t)sizeof(value);
            consume = context->consume;
            argument = context->consume_argument;
            context->consuming++;
        } else {
            if (UINT64_MAX - 1U - context->counter < value) return -EAGAIN;
            context->counter += value;
            return (int64_t)sizeof(value);
        }
    }
    // Called without the lock, which the consumer may take others after.
    consume(argument);
    EVENTFD_LOCKED;
    context->consuming--;
    return (int64_t)sizeof(uint64_t);
}

void eventfd_set_consumer(struct eventfd_context *context, void (*consume)(void *argument),
                          void *argument) {
    if (!context) return;
    for (;;) {
        {
            EVENTFD_LOCKED;
            if (consume || !context->consuming) {
                context->consume = consume;
                context->consume_argument = argument;
                if (consume) context->counter = 0;
                return;
            }
            context->consume = NULL;
        }
        cpu_relax();
    }
}

int eventfd_read_ready(struct eventfd_context *context) {
    EVENTFD_LOCKED;
    return context && context->counter != 0;
}

int eventfd_write_ready(struct eventfd_context *context) {
    EVENTFD_LOCKED;
    return context && context->counter < UINT64_MAX - 1U;
}

uint32_t eventfd_generation(struct eventfd_context *context) {
    EVENTFD_LOCKED;
    return context ? context->generation : 0;
}
