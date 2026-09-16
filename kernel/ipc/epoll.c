#include <stddef.h>
#include <stdint.h>
#include "../include/epoll.h"
#include "../include/file.h"
#include "../include/heap.h"
#include "../include/kstring.h"

#define EEXIST 17
#define EINVAL 22
#define ENOENT 2
#define ENOSPC 28
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)
#define EPOLLERR 0x008U
#define EPOLLHUP 0x010U
#define EPOLL_MAX_ENTRIES 128

struct epoll_entry {
    int active;
    int disarmed;
    int fd;
    struct file *file;
    uint32_t events;
    uint32_t edge_seen;
    uint32_t edge_generation;
    uint64_t data;
};

struct epoll_context {
    struct epoll_entry entries[EPOLL_MAX_ENTRIES];
};

static struct epoll_entry *find_entry(struct epoll_context *context, int fd,
                                      struct file *file) {
    if (!context || !file) return NULL;
    for (int index = 0; index < EPOLL_MAX_ENTRIES; index++) {
        struct epoll_entry *entry = &context->entries[index];
        if (entry->active && entry->fd == fd && entry->file == file) return entry;
    }
    return NULL;
}

static void arm_entry(struct epoll_entry *entry, const struct tunix_epoll_event *event) {
    entry->events = event->events;
    entry->data = event->data;
    entry->disarmed = 0;
    entry->edge_seen = 0;
    entry->edge_generation = entry->file->edge_generation;
}

static uint32_t entry_occurred(const struct epoll_entry *entry, unsigned depth) {
    uint32_t reportable = (entry->events & ~(EPOLLET | EPOLLONESHOT)) | EPOLLERR | EPOLLHUP;
    return file_poll_events_nested(entry->file, reportable, depth + 1) & reportable;
}

static int entry_fresh(const struct epoll_entry *entry, uint32_t occurred) {
    if (!occurred) return 0;
    if (!(entry->events & EPOLLET)) return 1;
    return (occurred & ~entry->edge_seen) != 0 ||
           entry->file->edge_generation != entry->edge_generation;
}

struct epoll_context *epoll_create(void) {
    struct epoll_context *context = kmalloc(sizeof(*context));
    if (!context) return NULL;
    memset(context, 0, sizeof(*context));
    return context;
}

void epoll_destroy(struct epoll_context *context) {
    if (!context) return;
    for (int index = 0; index < EPOLL_MAX_ENTRIES; index++) {
        if (context->entries[index].active && context->entries[index].file)
            file_unref(context->entries[index].file);
    }
    kfree(context);
}

int epoll_ctl_add(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event) {
    if (!context || !file || !event) return -EINVAL;
    if (find_entry(context, fd, file)) return -EEXIST;
    for (int index = 0; index < EPOLL_MAX_ENTRIES; index++) {
        struct epoll_entry *entry = &context->entries[index];
        if (!entry->active) {
            entry->active = 1;
            entry->fd = fd;
            entry->file = file;
            arm_entry(entry, event);
            file_ref(file);
            return 0;
        }
    }
    return -ENOSPC;
}

int epoll_ctl_mod(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event) {
    if (!context || !file || !event) return -EINVAL;
    struct epoll_entry *entry = find_entry(context, fd, file);
    if (!entry) return -ENOENT;
    arm_entry(entry, event);
    return 0;
}

int epoll_ctl_del(struct epoll_context *context, int fd, struct file *file) {
    if (!context || !file) return -EINVAL;
    struct epoll_entry *entry = find_entry(context, fd, file);
    if (!entry) return -ENOENT;
    file_unref(entry->file);
    memset(entry, 0, sizeof(*entry));
    return 0;
}

int epoll_collect(struct epoll_context *context,
                  struct tunix_epoll_event *events, int maximum, unsigned depth) {
    if (!context || !events || maximum <= 0) return -EINVAL;
    if (depth >= EPOLL_MAX_NESTING) return 0;
    int ready = 0;
    for (int index = 0; index < EPOLL_MAX_ENTRIES && ready < maximum; index++) {
        struct epoll_entry *entry = &context->entries[index];
        if (!entry->active || !entry->file || entry->disarmed) continue;
        uint32_t occurred = entry_occurred(entry, depth);
        int fresh = entry_fresh(entry, occurred);
        if (entry->events & EPOLLET) {
            entry->edge_seen = occurred;
            entry->edge_generation = entry->file->edge_generation;
        }
        if (!fresh) continue;
        events[ready].events = occurred;
        events[ready].data = entry->data;
        ready++;
        if (entry->events & EPOLLONESHOT) entry->disarmed = 1;
    }
    return ready;
}

int epoll_read_ready(struct epoll_context *context, unsigned depth) {
    if (!context) return 0;
    if (depth >= EPOLL_MAX_NESTING) return 0;
    for (int index = 0; index < EPOLL_MAX_ENTRIES; index++) {
        struct epoll_entry *entry = &context->entries[index];
        if (!entry->active || !entry->file || entry->disarmed) continue;
        if (entry_fresh(entry, entry_occurred(entry, depth))) return 1;
    }
    return 0;
}
