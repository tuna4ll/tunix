#ifndef TUNIX_EPOLL_H
#define TUNIX_EPOLL_H

#include <stddef.h>
#include <stdint.h>

struct epoll_context;
struct file;

#if defined(__x86_64__)
struct tunix_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));
#else
struct tunix_epoll_event {
    uint32_t events;
    uint64_t data;
};
#endif

struct epoll_context *epoll_create(void);
void epoll_destroy(struct epoll_context *context);
int epoll_ctl_add(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event);
int epoll_ctl_mod(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event);
int epoll_ctl_del(struct epoll_context *context, int fd, struct file *file);
#define EPOLL_MAX_NESTING 5

int epoll_collect(struct epoll_context *context,
                  struct tunix_epoll_event *events, int maximum, unsigned depth);
int epoll_read_ready(struct epoll_context *context, unsigned depth);

#endif
