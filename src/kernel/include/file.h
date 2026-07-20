#ifndef TUNIX_FILE_H
#define TUNIX_FILE_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct pipe_buffer;
struct unix_socket;
struct inet_socket;
struct netlink_socket;
struct pty_pair;
struct input_reader;
struct eventfd_context;
struct timerfd_context;
struct epoll_context;
struct inotify_context;
struct memfd_object;
struct signalfd_context;

#define FILE_KIND_VFS        1
#define FILE_KIND_PIPE_READ  2
#define FILE_KIND_PIPE_WRITE 3
#define FILE_KIND_SOCKET     4
#define FILE_KIND_PTY_MASTER 5
#define FILE_KIND_PTY_SLAVE  6
#define FILE_KIND_INET_SOCKET 7
#define FILE_KIND_INPUT       8
#define FILE_KIND_FRAMEBUFFER 9
#define FILE_KIND_EVENTFD     10
#define FILE_KIND_TIMERFD     11
#define FILE_KIND_EPOLL       12
#define FILE_KIND_INOTIFY     13
#define FILE_KIND_NETLINK_SOCKET 14
#define FILE_KIND_MEMFD       15
#define FILE_KIND_SIGNALFD    16

struct file {
    int refs;
    int kind;
    uint32_t flags;
    uint64_t offset;
    struct vfs_node *node;
    struct pipe_buffer *pipe;
    struct unix_socket *socket;
    struct inet_socket *inet_socket;
    struct netlink_socket *netlink_socket;
    struct pty_pair *pty;
    struct input_reader *input_reader;
    struct eventfd_context *eventfd;
    struct timerfd_context *timerfd;
    struct epoll_context *epoll;
    struct inotify_context *inotify;
    struct memfd_object *memfd;
    struct signalfd_context *signalfd;
    /* LOCK_SH or LOCK_EX while this open file description holds an advisory
       lock on file->node, 0 otherwise. */
    int flock_type;
};

struct file *file_open_node(struct vfs_node *node, uint32_t flags);
struct file *file_create_pipe_end(struct pipe_buffer *pipe, int write_end);
struct file *file_create_socket(struct unix_socket *socket);
struct file *file_create_inet_socket(struct inet_socket *socket);
struct file *file_create_netlink_socket(struct netlink_socket *socket);
struct file *file_create_eventfd(struct eventfd_context *context, uint32_t flags);
struct file *file_create_timerfd(struct timerfd_context *context, uint32_t flags);
struct file *file_create_epoll(struct epoll_context *context, uint32_t flags);
struct file *file_create_memfd(struct memfd_object *object, uint32_t flags);
struct file *file_create_signalfd(struct signalfd_context *context, uint32_t flags);
struct file *file_create_inotify(struct inotify_context *context, uint32_t flags);
struct file *file_create_pty_endpoint(struct pty_pair *pty, int master,
                                      struct vfs_node *node, uint32_t flags);
void file_ref(struct file *file);
void file_unref(struct file *file);

/* flock(2) operations, as the ABI defines them. */
#define FILE_LOCK_SH 1
#define FILE_LOCK_EX 2
#define FILE_LOCK_NB 4
#define FILE_LOCK_UN 8
/* Take or release an advisory lock. Returns 0 on success, -EWOULDBLOCK when the
   lock is contended, or another negative errno. */
int file_flock(struct file *file, int operation);
void file_flock_release(struct file *file);
int64_t file_read(struct file *file, size_t size, void *buffer);
int64_t file_write(struct file *file, size_t size, const void *buffer);
/*
 * Channel to sleep on when a read/write returned EAGAIN, or NULL when this file
 * kind has no wakeup source and the caller must fall back to retrying.
 */
const void *file_read_wait_channel(struct file *file);
const void *file_write_wait_channel(struct file *file);
uint32_t file_poll_events(struct file *file, uint32_t requested);

#endif
