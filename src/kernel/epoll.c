#include <stddef.h>
#include <stdint.h>
#include "include/epoll.h"
#include "include/file.h"
#include "include/heap.h"
#include "include/kstring.h"

#define EEXIST 17
#define EINVAL 22
#define ENOENT 2
#define ENOSPC 28
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)
#define EPOLLERR 0x008U
#define EPOLLHUP 0x010U
#define EPOLLRDHUP 0x2000U
#define EPOLL_MAX_ENTRIES 128

struct epoll_entry {
    int active;
    /* EPOLLONESHOT has fired. A disarmed entry reports *nothing* -- not even
       EPOLLERR/EPOLLHUP, which are otherwise always reported -- until an
       EPOLL_CTL_MOD re-arms it. dasynq (dinit's event library) leans on
       exactly this: it "disables" a watch by re-arming with events set to
       just EPOLLONESHOT, so a hung-up pipe under a disabled watch must fall
       silent after one report or every epoll_wait returns instantly and the
       caller's drain loop never terminates. */
    int disarmed;
    int fd;
    struct file *file;
    uint32_t events;
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
            entry->disarmed = 0;
            entry->fd = fd;
            entry->file = file;
            entry->events = event->events;
            entry->data = event->data;
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
    entry->events = event->events;
    entry->data = event->data;
    entry->disarmed = 0;
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
                  struct tunix_epoll_event *events, int maximum) {
    if (!context || !events || maximum <= 0) return -EINVAL;
    int ready = 0;
    for (int index = 0; index < EPOLL_MAX_ENTRIES && ready < maximum; index++) {
        struct epoll_entry *entry = &context->entries[index];
        if (!entry->active || !entry->file || entry->disarmed) continue;
        uint32_t requested = entry->events & ~(EPOLLET | EPOLLONESHOT);
        uint32_t occurred = file_poll_events(entry->file, requested | EPOLLERR |
                                                           EPOLLHUP | EPOLLRDHUP);
        occurred &= requested | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        if (!occurred) continue;
        events[ready].events = occurred;
        events[ready].data = entry->data;
        ready++;
        if (entry->events & EPOLLONESHOT) entry->disarmed = 1;
    }
    return ready;
}

int epoll_read_ready(struct epoll_context *context) {
    if (!context) return 0;
    for (int index = 0; index < EPOLL_MAX_ENTRIES; index++) {
        struct epoll_entry *entry = &context->entries[index];
        if (!entry->active || !entry->file || entry->disarmed) continue;
        uint32_t requested = entry->events & ~(EPOLLET | EPOLLONESHOT);
        uint32_t occurred = file_poll_events(entry->file, requested | EPOLLERR |
                                                           EPOLLHUP | EPOLLRDHUP);
        if (occurred & (requested | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) return 1;
    }
    return 0;
}
