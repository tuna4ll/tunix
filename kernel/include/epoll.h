#ifndef TUNIX_EPOLL_H
#define TUNIX_EPOLL_H

#include <stddef.h>
#include <stdint.h>

struct epoll_context;
struct file;

struct tunix_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct epoll_context *epoll_create(void);
void epoll_destroy(struct epoll_context *context);
int epoll_ctl_add(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event);
int epoll_ctl_mod(struct epoll_context *context, int fd, struct file *file,
                  const struct tunix_epoll_event *event);
int epoll_ctl_del(struct epoll_context *context, int fd, struct file *file);
/*
 * How far one epoll set may be asked about another before the answer is "no".
 *
 * An epoll set is itself pollable, so asking whether one is ready asks the
 * same of every descriptor in it -- and a descriptor in it can be another
 * epoll set. Nothing stops those from forming a ring, and a ring makes the
 * question unanswerable: the recursion has no bottom and walks the kernel
 * stack off the end of the heap, which the machine reports by resetting.
 *
 * Linux caps the nesting at 5 for exactly this reason. The depth is carried
 * through the call rather than checked when the set is built, because a ring
 * can be closed from either end and only the walk sees the whole of it.
 */
#define EPOLL_MAX_NESTING 5

int epoll_collect(struct epoll_context *context,
                  struct tunix_epoll_event *events, int maximum, unsigned depth);
int epoll_read_ready(struct epoll_context *context, unsigned depth);

#endif
