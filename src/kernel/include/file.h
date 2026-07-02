#ifndef TUNIX_FILE_H
#define TUNIX_FILE_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct pipe_buffer;
struct unix_socket;
struct inet_socket;
struct pty_pair;
struct input_reader;
struct eventfd_context;
struct timerfd_context;
struct epoll_context;
struct inotify_context;

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

struct file {
    int refs;
    int kind;
    uint32_t flags;
    uint64_t offset;
    struct vfs_node *node;
    struct pipe_buffer *pipe;
    struct unix_socket *socket;
    struct inet_socket *inet_socket;
    struct pty_pair *pty;
    struct input_reader *input_reader;
    struct eventfd_context *eventfd;
    struct timerfd_context *timerfd;
    struct epoll_context *epoll;
    struct inotify_context *inotify;
};

struct file *file_open_node(struct vfs_node *node, uint32_t flags);
struct file *file_create_pipe_end(struct pipe_buffer *pipe, int write_end);
struct file *file_create_socket(struct unix_socket *socket);
struct file *file_create_inet_socket(struct inet_socket *socket);
struct file *file_create_eventfd(struct eventfd_context *context, uint32_t flags);
struct file *file_create_timerfd(struct timerfd_context *context, uint32_t flags);
struct file *file_create_epoll(struct epoll_context *context, uint32_t flags);
struct file *file_create_inotify(struct inotify_context *context, uint32_t flags);
struct file *file_create_pty_endpoint(struct pty_pair *pty, int master,
                                      struct vfs_node *node, uint32_t flags);
void file_ref(struct file *file);
void file_unref(struct file *file);
int64_t file_read(struct file *file, size_t size, void *buffer);
int64_t file_write(struct file *file, size_t size, const void *buffer);
uint32_t file_poll_events(struct file *file, uint32_t requested);

#endif
