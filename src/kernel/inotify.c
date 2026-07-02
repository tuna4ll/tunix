#include <stddef.h>
#include <stdint.h>
#include "include/heap.h"
#include "include/inotify.h"
#include "include/kstring.h"

#define EAGAIN 11
#define EINVAL 22
#define ENOSPC 28
#define IN_IGNORED 0x00008000U
#define IN_Q_OVERFLOW 0x00004000U
#define INOTIFY_MAX_WATCHES 64
#define INOTIFY_QUEUE_SIZE 8192

struct linux_inotify_event {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t length;
};

struct inotify_watch {
    int active;
    int descriptor;
    uint32_t mask;
    struct vfs_node *node;
};

struct inotify_context {
    struct inotify_watch watches[INOTIFY_MAX_WATCHES];
    uint8_t queue[INOTIFY_QUEUE_SIZE];
    size_t read_position;
    size_t write_position;
    size_t queued;
    int next_descriptor;
    int overflow_reported;
    struct inotify_context *next;
};

static struct inotify_context *contexts;
static uint32_t next_cookie = 1;

static size_t aligned_name_length(const char *name) {
    if (!name || !name[0]) return 0;
    size_t length = strlen(name) + 1U;
    return (length + 3U) & ~3U;
}

static void queue_bytes(struct inotify_context *context, const void *data,
                        size_t size) {
    const uint8_t *bytes = data;
    for (size_t index = 0; index < size; index++) {
        context->queue[context->write_position] = bytes[index];
        context->write_position = (context->write_position + 1U) % INOTIFY_QUEUE_SIZE;
    }
    context->queued += size;
}

static int queue_event(struct inotify_context *context, int wd, uint32_t mask,
                       const char *name, uint32_t cookie) {
    size_t name_length = aligned_name_length(name);
    size_t total = sizeof(struct linux_inotify_event) + name_length;
    if (total > INOTIFY_QUEUE_SIZE - context->queued) {
        if (!context->overflow_reported &&
            sizeof(struct linux_inotify_event) <= INOTIFY_QUEUE_SIZE - context->queued) {
            struct linux_inotify_event overflow = {-1, IN_Q_OVERFLOW, 0, 0};
            queue_bytes(context, &overflow, sizeof(overflow));
            context->overflow_reported = 1;
        }
        return -ENOSPC;
    }
    struct linux_inotify_event event = {wd, mask, cookie, (uint32_t)name_length};
    queue_bytes(context, &event, sizeof(event));
    if (name_length) {
        char padded[260];
        if (name_length > sizeof(padded)) return -EINVAL;
        memset(padded, 0, name_length);
        strncpy(padded, name, name_length - 1U);
        queue_bytes(context, padded, name_length);
    }
    return 0;
}

struct inotify_context *inotify_create(void) {
    struct inotify_context *context = kmalloc(sizeof(*context));
    if (!context) return NULL;
    memset(context, 0, sizeof(*context));
    context->next_descriptor = 1;
    context->next = contexts;
    contexts = context;
    return context;
}

void inotify_destroy(struct inotify_context *context) {
    if (!context) return;
    struct inotify_context **link = &contexts;
    while (*link && *link != context) link = &(*link)->next;
    if (*link == context) *link = context->next;
    kfree(context);
}

int inotify_add_watch(struct inotify_context *context, struct vfs_node *node,
                      uint32_t mask) {
    if (!context || !node || !mask) return -EINVAL;
    for (int index = 0; index < INOTIFY_MAX_WATCHES; index++) {
        struct inotify_watch *watch = &context->watches[index];
        if (watch->active && watch->node == node) {
            watch->mask = mask;
            return watch->descriptor;
        }
    }
    for (int index = 0; index < INOTIFY_MAX_WATCHES; index++) {
        struct inotify_watch *watch = &context->watches[index];
        if (!watch->active) {
            watch->active = 1;
            watch->descriptor = context->next_descriptor++;
            if (context->next_descriptor <= 0) context->next_descriptor = 1;
            watch->mask = mask;
            watch->node = node;
            return watch->descriptor;
        }
    }
    return -ENOSPC;
}

int inotify_remove_watch(struct inotify_context *context, int descriptor) {
    if (!context || descriptor <= 0) return -EINVAL;
    for (int index = 0; index < INOTIFY_MAX_WATCHES; index++) {
        struct inotify_watch *watch = &context->watches[index];
        if (watch->active && watch->descriptor == descriptor) {
            (void)queue_event(context, descriptor, IN_IGNORED, NULL, 0);
            memset(watch, 0, sizeof(*watch));
            return 0;
        }
    }
    return -EINVAL;
}

static void peek_bytes(const struct inotify_context *context, size_t offset,
                       void *buffer, size_t size) {
    uint8_t *output = buffer;
    size_t position = (context->read_position + offset) % INOTIFY_QUEUE_SIZE;
    for (size_t index = 0; index < size; index++) {
        output[index] = context->queue[position];
        position = (position + 1U) % INOTIFY_QUEUE_SIZE;
    }
}

int64_t inotify_read(struct inotify_context *context, size_t size, void *buffer) {
    if (!context || !buffer) return -EINVAL;
    if (!context->queued) return -EAGAIN;
    if (size < sizeof(struct linux_inotify_event)) return -EINVAL;

    uint8_t *output = buffer;
    size_t copied = 0;
    while (context->queued >= sizeof(struct linux_inotify_event)) {
        struct linux_inotify_event event;
        peek_bytes(context, 0, &event, sizeof(event));
        size_t event_size = sizeof(event) + event.length;
        if (event_size < sizeof(event) || event_size > context->queued ||
            event_size > INOTIFY_QUEUE_SIZE)
            return copied ? (int64_t)copied : -EINVAL;
        if (event_size > size - copied) {
            if (!copied) return -EINVAL;
            break;
        }
        peek_bytes(context, 0, output + copied, event_size);
        context->read_position =
            (context->read_position + event_size) % INOTIFY_QUEUE_SIZE;
        context->queued -= event_size;
        copied += event_size;
        if (copied == size) break;
    }
    if (!context->queued) context->overflow_reported = 0;
    return (int64_t)copied;
}

int inotify_read_ready(struct inotify_context *context) {
    return context && context->queued != 0;
}

void inotify_notify(struct vfs_node *node, uint32_t mask, const char *name,
                    uint32_t cookie) {
    if (!node || !mask) return;
    for (struct inotify_context *context = contexts; context; context = context->next) {
        for (int index = 0; index < INOTIFY_MAX_WATCHES; index++) {
            struct inotify_watch *watch = &context->watches[index];
            if (watch->active && watch->node == node && (watch->mask & mask))
                (void)queue_event(context, watch->descriptor, mask & watch->mask,
                                  name, cookie);
        }
    }
}

void inotify_invalidate(struct vfs_node *node) {
    if (!node) return;
    for (struct inotify_context *context = contexts; context; context = context->next) {
        for (int index = 0; index < INOTIFY_MAX_WATCHES; index++) {
            struct inotify_watch *watch = &context->watches[index];
            if (watch->active && watch->node == node) {
                (void)queue_event(context, watch->descriptor, IN_IGNORED, NULL, 0);
                memset(watch, 0, sizeof(*watch));
            }
        }
    }
}

uint32_t inotify_next_cookie(void) {
    uint32_t cookie = next_cookie++;
    if (!next_cookie) next_cookie = 1;
    return cookie;
}
