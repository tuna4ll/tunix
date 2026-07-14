#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/heap.h"
#include "include/framebuffer.h"
#include "include/eventfd.h"
#include "include/timerfd.h"
#include "include/epoll.h"
#include "include/inotify.h"
#include "include/input.h"
#include "include/pipe.h"
#include "include/pty.h"
#include "include/vfs.h"
#include "include/unix_socket.h"
#include "include/net/inet_socket.h"

#define EAGAIN 11
#define EBADF 9
#define EPIPE 32

struct file *file_open_node(struct vfs_node *node, uint32_t flags) {
    if (!node) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    file->refs = 1;
    file->kind = FILE_KIND_VFS;
    file->flags = flags;
    file->offset = 0;
    file->node = node;
    file->pipe = NULL;
    file->socket = NULL;
    file->inet_socket = NULL;
    file->pty = NULL;
    file->input_reader = NULL;
    file->eventfd = NULL;
    file->timerfd = NULL;
    file->epoll = NULL;
    file->inotify = NULL;
    if (node->flags & VFS_FRAMEBUFFER) {
        file->kind = FILE_KIND_FRAMEBUFFER;
    } else if (node->flags & VFS_INPUTDEVICE) {
        unsigned device_id = (unsigned)(uintptr_t)node->data;
        file->input_reader = input_reader_open(device_id);
        if (!file->input_reader) {
            kfree(file);
            return NULL;
        }
        file->kind = FILE_KIND_INPUT;
    } else if (node->open) {
        node->open(node);
    }
    return file;
}

struct file *file_create_pipe_end(struct pipe_buffer *pipe, int write_end) {
    if (!pipe) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    file->refs = 1;
    file->kind = write_end ? FILE_KIND_PIPE_WRITE : FILE_KIND_PIPE_READ;
    file->flags = 0;
    file->offset = 0;
    file->node = NULL;
    file->pipe = pipe;
    file->socket = NULL;
    file->inet_socket = NULL;
    file->pty = NULL;
    file->input_reader = NULL;
    file->eventfd = NULL;
    file->timerfd = NULL;
    file->epoll = NULL;
    file->inotify = NULL;
    if (write_end) pipe->writers++;
    else pipe->readers++;
    return file;
}


struct file *file_create_socket(struct unix_socket *socket) {
    if (!socket) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    file->refs = 1;
    file->kind = FILE_KIND_SOCKET;
    file->flags = 0;
    file->offset = 0;
    file->node = NULL;
    file->pipe = NULL;
    file->socket = socket;
    file->inet_socket = NULL;
    file->pty = NULL;
    file->input_reader = NULL;
    file->eventfd = NULL;
    file->timerfd = NULL;
    file->epoll = NULL;
    file->inotify = NULL;
    return file;
}

struct file *file_create_inet_socket(struct inet_socket *socket) {
    if (!socket) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    file->refs = 1;
    file->kind = FILE_KIND_INET_SOCKET;
    file->flags = 0;
    file->offset = 0;
    file->node = NULL;
    file->pipe = NULL;
    file->socket = NULL;
    file->inet_socket = socket;
    file->pty = NULL;
    file->input_reader = NULL;
    file->eventfd = NULL;
    file->timerfd = NULL;
    file->epoll = NULL;
    file->inotify = NULL;
    return file;
}

static struct file *file_create_special(int kind, uint32_t flags) {
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    file->refs = 1;
    file->kind = kind;
    file->flags = flags;
    file->offset = 0;
    file->node = NULL;
    file->pipe = NULL;
    file->socket = NULL;
    file->inet_socket = NULL;
    file->pty = NULL;
    file->input_reader = NULL;
    file->eventfd = NULL;
    file->timerfd = NULL;
    file->epoll = NULL;
    file->inotify = NULL;
    return file;
}

struct file *file_create_eventfd(struct eventfd_context *context, uint32_t flags) {
    struct file *file = file_create_special(FILE_KIND_EVENTFD, flags);
    if (file) file->eventfd = context;
    return file;
}

struct file *file_create_timerfd(struct timerfd_context *context, uint32_t flags) {
    struct file *file = file_create_special(FILE_KIND_TIMERFD, flags);
    if (file) file->timerfd = context;
    return file;
}

struct file *file_create_epoll(struct epoll_context *context, uint32_t flags) {
    struct file *file = file_create_special(FILE_KIND_EPOLL, flags);
    if (file) file->epoll = context;
    return file;
}

struct file *file_create_inotify(struct inotify_context *context, uint32_t flags) {
    struct file *file = file_create_special(FILE_KIND_INOTIFY, flags);
    if (file) file->inotify = context;
    return file;
}

struct file *file_create_pty_endpoint(struct pty_pair *pty, int master,
                                      struct vfs_node *node, uint32_t flags) {
    if (!pty || !node) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    file->refs = 1;
    file->kind = master ? FILE_KIND_PTY_MASTER : FILE_KIND_PTY_SLAVE;
    file->flags = flags;
    file->offset = 0;
    file->node = node;
    file->pipe = NULL;
    file->socket = NULL;
    file->inet_socket = NULL;
    file->pty = pty;
    file->input_reader = NULL;
    file->eventfd = NULL;
    file->timerfd = NULL;
    file->epoll = NULL;
    file->inotify = NULL;
    return file;
}

void file_ref(struct file *file) {
    if (file) file->refs++;
}

void file_unref(struct file *file) {
    if (!file || file->refs <= 0) return;
    file->refs--;
    if (file->refs != 0) return;
    if (file->kind == FILE_KIND_PIPE_READ && file->pipe && file->pipe->readers > 0) file->pipe->readers--;
    if (file->kind == FILE_KIND_PIPE_WRITE && file->pipe && file->pipe->writers > 0) file->pipe->writers--;
    if (file->kind == FILE_KIND_VFS && file->node && file->node->close)
        file->node->close(file->node);
    if (file->kind == FILE_KIND_INPUT && file->input_reader)
        input_reader_close(file->input_reader);
    if (file->kind == FILE_KIND_FRAMEBUFFER)
        framebuffer_file_close(file);
    if (file->kind == FILE_KIND_EVENTFD && file->eventfd)
        eventfd_destroy(file->eventfd);
    if (file->kind == FILE_KIND_TIMERFD && file->timerfd)
        timerfd_destroy(file->timerfd);
    if (file->kind == FILE_KIND_EPOLL && file->epoll)
        epoll_destroy(file->epoll);
    if (file->kind == FILE_KIND_INOTIFY && file->inotify)
        inotify_destroy(file->inotify);
    if (file->kind == FILE_KIND_SOCKET && file->socket)
        unix_socket_unref(file->socket);
    if (file->kind == FILE_KIND_INET_SOCKET && file->inet_socket)
        inet_socket_unref(file->inet_socket);
    if ((file->kind == FILE_KIND_PTY_MASTER || file->kind == FILE_KIND_PTY_SLAVE) && file->pty)
        pty_close_endpoint(file->pty, file->kind == FILE_KIND_PTY_MASTER);
    kfree(file);
}

int64_t file_read(struct file *file, size_t size, void *buffer) {
    if (!file || !buffer) return -EBADF;
    if (file->kind == FILE_KIND_PIPE_READ) return pipe_read(file->pipe, size, buffer);
    if (file->kind == FILE_KIND_SOCKET) return unix_socket_read(file->socket, size, buffer);
    if (file->kind == FILE_KIND_INET_SOCKET) return inet_socket_read(file->inet_socket, size, buffer);
    if (file->kind == FILE_KIND_PTY_MASTER || file->kind == FILE_KIND_PTY_SLAVE)
        return pty_read(file->pty, file->kind == FILE_KIND_PTY_MASTER, size, buffer);
    if (file->kind == FILE_KIND_INPUT)
        return input_reader_read(file->input_reader, size, buffer);
    if (file->kind == FILE_KIND_FRAMEBUFFER)
        return framebuffer_file_read(file, size, buffer);
    if (file->kind == FILE_KIND_EVENTFD)
        return eventfd_read(file->eventfd, size, buffer);
    if (file->kind == FILE_KIND_TIMERFD)
        return timerfd_read(file->timerfd, size, buffer);
    if (file->kind == FILE_KIND_INOTIFY)
        return inotify_read(file->inotify, size, buffer);
    if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    int64_t result = vfs_read(file->node, file->offset, size, buffer);
    if (result > 0) file->offset += (uint64_t)result;
    return result;
}

int64_t file_write(struct file *file, size_t size, const void *buffer) {
    if (!file || !buffer) return -EBADF;
    if (file->kind == FILE_KIND_SOCKET) return unix_socket_write(file->socket, size, buffer);
    if (file->kind == FILE_KIND_INET_SOCKET) return inet_socket_write(file->inet_socket, size, buffer);
    if (file->kind == FILE_KIND_PTY_MASTER || file->kind == FILE_KIND_PTY_SLAVE)
        return pty_write(file->pty, file->kind == FILE_KIND_PTY_MASTER, size, buffer);
    if (file->kind == FILE_KIND_PIPE_WRITE) {
        if (file->pipe && file->pipe->readers == 0) return -EPIPE;
        return pipe_write(file->pipe, size, buffer);
    }
    if (file->kind == FILE_KIND_FRAMEBUFFER)
        return framebuffer_file_write(file, size, buffer);
    if (file->kind == FILE_KIND_EVENTFD)
        return eventfd_write(file->eventfd, size, buffer);
    if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    int64_t result = vfs_write(file->node, file->offset, size, buffer);
    if (result > 0) file->offset += (uint64_t)result;
    return result;
}

uint32_t file_poll_events(struct file *file, uint32_t requested) {
    const uint32_t pollin = 0x001U;
    const uint32_t pollout = 0x004U;
    const uint32_t pollerr = 0x008U;
    const uint32_t pollhup = 0x010U;
    const uint32_t pollrdhup = 0x2000U;
    if (!file) return pollerr;
    uint32_t events = 0;
    if (file->kind == FILE_KIND_PIPE_READ) {
        if (file->pipe && (file->pipe->count > 0 || file->pipe->writers == 0)) events |= pollin;
        if (file->pipe && file->pipe->writers == 0) events |= pollhup;
    } else if (file->kind == FILE_KIND_PIPE_WRITE) {
        if (!file->pipe || file->pipe->readers == 0) events |= pollerr;
        else if (file->pipe->count < PIPE_CAPACITY) events |= pollout;
    } else if (file->kind == FILE_KIND_SOCKET) {
        if (unix_socket_read_ready(file->socket)) events |= pollin;
        if (unix_socket_write_ready(file->socket)) events |= pollout;
        if (unix_socket_peer_closed(file->socket)) events |= pollhup | pollrdhup;
    } else if (file->kind == FILE_KIND_INET_SOCKET) {
        if (inet_socket_read_ready(file->inet_socket)) events |= pollin;
        if (inet_socket_write_ready(file->inet_socket)) events |= pollout;
        if (inet_socket_peer_closed(file->inet_socket)) events |= pollhup | pollrdhup;
    } else if (file->kind == FILE_KIND_PTY_MASTER || file->kind == FILE_KIND_PTY_SLAVE) {
        int master = file->kind == FILE_KIND_PTY_MASTER;
        if (pty_read_ready(file->pty, master)) events |= pollin;
        if (pty_write_ready(file->pty, master)) events |= pollout;
    } else if (file->kind == FILE_KIND_INPUT) {
        if (input_reader_ready(file->input_reader)) events |= pollin;
    } else if (file->kind == FILE_KIND_FRAMEBUFFER) {
        events |= pollin | pollout;
    } else if (file->kind == FILE_KIND_EVENTFD) {
        if (eventfd_read_ready(file->eventfd)) events |= pollin;
        if (eventfd_write_ready(file->eventfd)) events |= pollout;
    } else if (file->kind == FILE_KIND_TIMERFD) {
        if (timerfd_read_ready(file->timerfd)) events |= pollin;
    } else if (file->kind == FILE_KIND_EPOLL) {
        if (epoll_read_ready(file->epoll)) events |= pollin;
    } else if (file->kind == FILE_KIND_INOTIFY) {
        if (inotify_read_ready(file->inotify)) events |= pollin;
    } else if (file->kind == FILE_KIND_VFS && file->node) {
        if (file->node->read_ready ? file->node->read_ready(file->node) :
            ((file->node->flags & 0xFFU) != VFS_CHARDEVICE)) events |= pollin;
        events |= pollout;
    } else {
        events |= pollerr;
    }
    return events & (requested | pollerr | pollhup | pollrdhup);
}
