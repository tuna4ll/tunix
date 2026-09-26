#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/inotify.h"
#include "../include/kstring.h"

#define EAGAIN 11
#define EINVAL 22
#define ENOSPC 28
#define IN_IGNORED 0x00008000U
#define IN_Q_OVERFLOW 0x00004000U
#define INOTIFY_QUEUE_MIN 8192U
#define INOTIFY_QUEUE_MAX (1024U * 1024U)
#define INOTIFY_BUCKETS 256U

struct linux_inotify_event {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t length;
};

struct inotify_watch {
    int descriptor;
    uint32_t mask;
    struct vfs_node *node;
    struct inotify_context *context;
    struct inotify_watch *node_next;
    struct inotify_watch *context_next;
};

struct inotify_context {
    struct inotify_watch *watches;
    uint8_t *queue;
    size_t capacity;
    size_t read_position;
    size_t write_position;
    size_t queued;
    int next_descriptor;
    int overflow_reported;
};

static struct inotify_watch *node_buckets[INOTIFY_BUCKETS];
static uint32_t next_cookie = 1;

static struct inotify_watch **node_bucket(const struct vfs_node *node) {
    uint64_t value = (uint64_t)(uintptr_t)node;
    return &node_buckets[((value >> 4) * 0x9E3779B97F4A7C15ULL >> 56) & (INOTIFY_BUCKETS - 1U)];
}

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
        context->write_position = (context->write_position + 1U) % context->capacity;
    }
    context->queued += size;
}

static void peek_bytes(const struct inotify_context *context, size_t offset,
                       void *buffer, size_t size) {
    uint8_t *output = buffer;
    size_t position = (context->read_position + offset) % context->capacity;
    for (size_t index = 0; index < size; index++) {
        output[index] = context->queue[position];
        position = (position + 1U) % context->capacity;
    }
}

static int grow_queue(struct inotify_context *context, size_t needed) {
    size_t capacity = context->capacity;
    while (capacity - context->queued < needed && capacity < INOTIFY_QUEUE_MAX) capacity *= 2;
    if (capacity - context->queued < needed) return -1;
    uint8_t *queue = kmalloc(capacity);
    if (!queue) return -1;
    peek_bytes(context, 0, queue, context->queued);
    kfree(context->queue);
    context->queue = queue;
    context->capacity = capacity;
    context->read_position = 0;
    context->write_position = context->queued;
    return 0;
}

static int queue_event(struct inotify_context *context, int wd, uint32_t mask,
                       const char *name, uint32_t cookie) {
    size_t name_length = aligned_name_length(name);
    size_t total = sizeof(struct linux_inotify_event) + name_length;
    if (total > context->capacity - context->queued && grow_queue(context, total) != 0) {
        if (!context->overflow_reported &&
            sizeof(struct linux_inotify_event) <= context->capacity - context->queued) {
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
    context->queue = kmalloc(INOTIFY_QUEUE_MIN);
    if (!context->queue) {
        kfree(context);
        return NULL;
    }
    context->capacity = INOTIFY_QUEUE_MIN;
    context->next_descriptor = 1;
    return context;
}

static void unlink_from_node(struct inotify_watch *watch) {
    for (struct inotify_watch **link = node_bucket(watch->node); *link;
         link = &(*link)->node_next) {
        if (*link == watch) {
            *link = watch->node_next;
            return;
        }
    }
}

static void drop_watch(struct inotify_watch *watch) {
    struct inotify_context *context = watch->context;
    for (struct inotify_watch **link = &context->watches; *link;
         link = &(*link)->context_next) {
        if (*link == watch) {
            *link = watch->context_next;
            break;
        }
    }
    unlink_from_node(watch);
    kfree(watch);
}

void inotify_destroy(struct inotify_context *context) {
    if (!context) return;
    while (context->watches) drop_watch(context->watches);
    kfree(context->queue);
    kfree(context);
}

int inotify_add_watch(struct inotify_context *context, struct vfs_node *node,
                      uint32_t mask) {
    if (!context || !node || !mask) return -EINVAL;
    for (struct inotify_watch *watch = *node_bucket(node); watch; watch = watch->node_next) {
        if (watch->context == context && watch->node == node) {
            watch->mask = mask;
            return watch->descriptor;
        }
    }
    struct inotify_watch *watch = kmalloc(sizeof(*watch));
    if (!watch) return -ENOSPC;
    watch->descriptor = context->next_descriptor++;
    if (context->next_descriptor <= 0) context->next_descriptor = 1;
    watch->mask = mask;
    watch->node = node;
    watch->context = context;
    watch->context_next = context->watches;
    context->watches = watch;
    struct inotify_watch **bucket = node_bucket(node);
    watch->node_next = *bucket;
    *bucket = watch;
    return watch->descriptor;
}

int inotify_remove_watch(struct inotify_context *context, int descriptor) {
    if (!context || descriptor <= 0) return -EINVAL;
    for (struct inotify_watch *watch = context->watches; watch; watch = watch->context_next) {
        if (watch->descriptor == descriptor) {
            (void)queue_event(context, descriptor, IN_IGNORED, NULL, 0);
            drop_watch(watch);
            return 0;
        }
    }
    return -EINVAL;
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
        if (event_size < sizeof(event) || event_size > context->queued)
            return copied ? (int64_t)copied : -EINVAL;
        if (event_size > size - copied) {
            if (!copied) return -EINVAL;
            break;
        }
        peek_bytes(context, 0, output + copied, event_size);
        context->read_position = (context->read_position + event_size) % context->capacity;
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
    for (struct inotify_watch *watch = *node_bucket(node); watch; watch = watch->node_next)
        if (watch->node == node && (watch->mask & mask))
            (void)queue_event(watch->context, watch->descriptor, mask & watch->mask,
                              name, cookie);
}

void inotify_invalidate(struct vfs_node *node) {
    if (!node) return;
    struct inotify_watch *watch = *node_bucket(node);
    while (watch) {
        struct inotify_watch *next = watch->node_next;
        if (watch->node == node) {
            (void)queue_event(watch->context, watch->descriptor, IN_IGNORED, NULL, 0);
            drop_watch(watch);
        }
        watch = next;
    }
}

uint32_t inotify_next_cookie(void) {
    uint32_t cookie = next_cookie++;
    if (!next_cookie) next_cookie = 1;
    return cookie;
}
