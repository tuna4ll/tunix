#include <stddef.h>
#include <stdint.h>
#include <tunix/epoll.h>
#include <tunix/file.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/syscall.h>

static struct lock epoll_lock = LOCK_INITIALIZER("epoll", LOCK_RANK_OBJECT);

static void epoll_guard_release(int *unused) {
    (void)unused;
    lock_release(&epoll_lock);
}

#define EPOLL_LOCKED \
    __attribute__((cleanup(epoll_guard_release))) int epoll_guard = (lock_acquire(&epoll_lock), 0)

#define EEXIST 17
#define EINVAL 22
#define ENOENT 2
#define ENOSPC 28
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)
#define EPOLLERR 0x008U
#define EPOLLHUP 0x010U
#define EPOLL_BUCKETS 64U

struct epoll_entry {
    int active;
    int disarmed;
    int fd;
    int hash_next;
    struct file *file;
    uint32_t events;
    uint32_t edge_seen;
    uint32_t edge_generation;
    uint64_t data;
};

struct epoll_context {
    struct epoll_entry *entries;
    int capacity;
    int count;
    int free_hint;
    int cursor;
    int buckets[EPOLL_BUCKETS];
};

static unsigned fd_bucket(int fd) {
    return (unsigned)fd % EPOLL_BUCKETS;
}

static struct epoll_entry *find_entry(struct epoll_context *context, int fd,
                                      struct file *file) {
    if (!context || !file) return NULL;
    for (int index = context->buckets[fd_bucket(fd)]; index >= 0;
         index = context->entries[index].hash_next) {
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
    EPOLL_LOCKED;
    struct epoll_context *context = kmalloc(sizeof(*context));
    if (!context) return NULL;
    memset(context, 0, sizeof(*context));
    for (unsigned bucket = 0; bucket < EPOLL_BUCKETS; bucket++) context->buckets[bucket] = -1;
    return context;
}

void epoll_destroy(struct epoll_context *context) {
    EPOLL_LOCKED;
    if (!context) return;
    for (int index = 0; index < context->capacity; index++) {
        if (context->entries[index].active && context->entries[index].file)
            syscall_unref_later(context->entries[index].file);
    }
    kfree(context->entries);
    kfree(context);
}

static int free_slot(struct epoll_context *context) {
    for (int index = context->free_hint; index < context->capacity; index++)
        if (!context->entries[index].active) return index;
    int capacity = context->capacity ? context->capacity * 2 : 16;
    struct epoll_entry *entries = kmalloc((size_t)capacity * sizeof(*entries));
    if (!entries) return -1;
    memset(entries, 0, (size_t)capacity * sizeof(*entries));
    if (context->capacity)
        memcpy(entries, context->entries, (size_t)context->capacity * sizeof(*entries));
    kfree(context->entries);
    int first = context->capacity;
    context->entries = entries;
    context->capacity = capacity;
    return first;
}

int epoll_entry_count(const struct epoll_context *context) {
    EPOLL_LOCKED;
    return context ? context->count : 0;
}

int epoll_ctl_add(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event) {
    EPOLL_LOCKED;
    if (!context || !file || !event) return -EINVAL;
    if (find_entry(context, fd, file)) return -EEXIST;
    int index = free_slot(context);
    if (index < 0) return -ENOSPC;
    struct epoll_entry *entry = &context->entries[index];
    entry->active = 1;
    entry->fd = fd;
    entry->file = file;
    entry->hash_next = context->buckets[fd_bucket(fd)];
    context->buckets[fd_bucket(fd)] = index;
    arm_entry(entry, event);
    file_ref(file);
    context->count++;
    context->free_hint = index + 1;
    return 0;
}

int epoll_ctl_mod(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event) {
    EPOLL_LOCKED;
    if (!context || !file || !event) return -EINVAL;
    struct epoll_entry *entry = find_entry(context, fd, file);
    if (!entry) return -ENOENT;
    arm_entry(entry, event);
    return 0;
}

int epoll_ctl_del(struct epoll_context *context, int fd, struct file *file) {
    EPOLL_LOCKED;
    if (!context || !file) return -EINVAL;
    struct epoll_entry *entry = find_entry(context, fd, file);
    if (!entry) return -ENOENT;
    int index = (int)(entry - context->entries);
    for (int *link = &context->buckets[fd_bucket(fd)]; *link >= 0;
         link = &context->entries[*link].hash_next) {
        if (*link == index) {
            *link = entry->hash_next;
            break;
        }
    }
    syscall_unref_later(entry->file);
    memset(entry, 0, sizeof(*entry));
    context->count--;
    if (index < context->free_hint) context->free_hint = index;
    return 0;
}

int epoll_collect(struct epoll_context *context,
                  struct tunix_epoll_event *events, int maximum, unsigned depth) {
    EPOLL_LOCKED;
    if (!context || !events || maximum <= 0) return -EINVAL;
    if (depth >= EPOLL_MAX_NESTING) return 0;
    int ready = 0;
    int start = context->capacity ? context->cursor % context->capacity : 0;
    for (int step = 0; step < context->capacity && ready < maximum; step++) {
        int index = (start + step) % context->capacity;
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
        if (ready == maximum) context->cursor = index + 1;
    }
    return ready;
}

int epoll_read_ready(struct epoll_context *context, unsigned depth) {
    EPOLL_LOCKED;
    if (!context) return 0;
    if (depth >= EPOLL_MAX_NESTING) return 0;
    for (int index = 0; index < context->capacity; index++) {
        struct epoll_entry *entry = &context->entries[index];
        if (!entry->active || !entry->file || entry->disarmed) continue;
        if (entry_fresh(entry, entry_occurred(entry, depth))) return 1;
    }
    return 0;
}
