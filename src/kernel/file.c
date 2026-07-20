#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/heap.h"
#include "include/kstring.h"
#include "include/framebuffer.h"
#include "include/eventfd.h"
#include "include/timerfd.h"
#include "include/epoll.h"
#include "include/inotify.h"
#include "include/input.h"
#include "include/memfd.h"
#include "include/signalfd.h"
#include "include/pipe.h"
#include "include/pty.h"
#include "include/vfs.h"
#include "include/unix_socket.h"
#include "include/net/inet_socket.h"
#include "include/net/netlink.h"

#define EAGAIN 11
#define EBADF 9
#define EINVAL 22
/* EWOULDBLOCK is EAGAIN on Linux, and flock reports contention with it. */
#define EWOULDBLOCK EAGAIN
#define EPIPE 32

struct file *file_open_node(struct vfs_node *node, uint32_t flags) {
    if (!node) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    memset(file, 0, sizeof(*file));
    file->refs = 1;
    file->kind = FILE_KIND_VFS;
    file->flags = flags;
    file->node = node;
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
    memset(file, 0, sizeof(*file));
    file->refs = 1;
    file->kind = write_end ? FILE_KIND_PIPE_WRITE : FILE_KIND_PIPE_READ;
    file->flags = 0;
    file->pipe = pipe;
    if (write_end) pipe->writers++;
    else pipe->readers++;
    return file;
}


struct file *file_create_socket(struct unix_socket *socket) {
    if (!socket) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    memset(file, 0, sizeof(*file));
    file->refs = 1;
    file->kind = FILE_KIND_SOCKET;
    file->flags = 0;
    file->socket = socket;
    return file;
}

struct file *file_create_inet_socket(struct inet_socket *socket) {
    if (!socket) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    memset(file, 0, sizeof(*file));
    file->refs = 1;
    file->kind = FILE_KIND_INET_SOCKET;
    file->flags = 0;
    file->inet_socket = socket;
    return file;
}

struct file *file_create_netlink_socket(struct netlink_socket *socket) {
    if (!socket) return NULL;
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    memset(file, 0, sizeof(*file));
    file->refs = 1;
    file->kind = FILE_KIND_NETLINK_SOCKET;
    file->flags = 0;
    file->netlink_socket = socket;
    return file;
}

static struct file *file_create_special(int kind, uint32_t flags) {
    struct file *file = (struct file *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    memset(file, 0, sizeof(*file));
    file->refs = 1;
    file->kind = kind;
    file->flags = flags;
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

struct file *file_create_memfd(struct memfd_object *object, uint32_t flags) {
    struct file *file = file_create_special(FILE_KIND_MEMFD, flags);
    if (file) file->memfd = object;
    return file;
}

struct file *file_create_signalfd(struct signalfd_context *context, uint32_t flags) {
    struct file *file = file_create_special(FILE_KIND_SIGNALFD, flags);
    if (file) file->signalfd = context;
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
    memset(file, 0, sizeof(*file));
    file->refs = 1;
    file->kind = master ? FILE_KIND_PTY_MASTER : FILE_KIND_PTY_SLAVE;
    file->flags = flags;
    file->node = node;
    file->pty = pty;
    return file;
}

const void *file_read_wait_channel(struct file *file) {
    if (file && file->kind == FILE_KIND_PIPE_READ && file->pipe)
        return &file->pipe->data_wait;
    return NULL;
}

const void *file_write_wait_channel(struct file *file) {
    if (file && file->kind == FILE_KIND_PIPE_WRITE && file->pipe)
        return &file->pipe->space_wait;
    return NULL;
}

/*
 * Advisory whole-file locking, flock(2) style.
 *
 * The lock belongs to the *open file description*, not to the descriptor or the
 * process, which is why the holder is this struct file: dup() and fork() share
 * one, so they share the lock, while a second open() of the same path gets its
 * own and contends. libwayland relies on exactly that to stop two compositors
 * claiming the same socket name.
 */
void file_flock_release(struct file *file) {
    if (!file || !file->flock_type || !file->node) {
        if (file) file->flock_type = 0;
        return;
    }
    if (file->flock_type == FILE_LOCK_EX) {
        if (file->node->flock_exclusive == file) file->node->flock_exclusive = NULL;
    } else if (file->node->flock_shared) {
        file->node->flock_shared--;
    }
    file->flock_type = 0;
}

int file_flock(struct file *file, int operation) {
    if (!file) return -EBADF;
    /* Only node-backed descriptors carry a lockable identity here; a pipe or
       socket has no shared object for a second opener to contend with. */
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;

    int mode = operation & ~FILE_LOCK_NB;
    struct vfs_node *node = file->node;

    if (mode == FILE_LOCK_UN) {
        file_flock_release(file);
        return 0;
    }
    if (mode != FILE_LOCK_SH && mode != FILE_LOCK_EX) return -EINVAL;

    /* Someone else's exclusive lock blocks both kinds of request. */
    if (node->flock_exclusive && node->flock_exclusive != file) return -EWOULDBLOCK;

    if (mode == FILE_LOCK_EX) {
        /* Any shared holder other than ourselves blocks an exclusive lock. */
        uint32_t others = node->flock_shared;
        if (file->flock_type == FILE_LOCK_SH && others) others--;
        if (others) return -EWOULDBLOCK;
        file_flock_release(file);
        node->flock_exclusive = file;
        file->flock_type = FILE_LOCK_EX;
        return 0;
    }

    /* Shared: re-taking it is a no-op, and downgrading from exclusive is
       allowed without ever dropping the lock in between. */
    if (file->flock_type == FILE_LOCK_SH) return 0;
    file_flock_release(file);
    node->flock_shared++;
    file->flock_type = FILE_LOCK_SH;
    return 0;
}

void file_ref(struct file *file) {
    if (file) file->refs++;
}

void file_unref(struct file *file) {
    if (!file || file->refs <= 0) return;
    file->refs--;
    if (file->refs != 0) return;
    /* The last descriptor referring to this open file description is going
       away, which is exactly when flock releases its lock. */
    file_flock_release(file);
    if ((file->kind == FILE_KIND_PIPE_READ || file->kind == FILE_KIND_PIPE_WRITE) && file->pipe)
        pipe_release(file->pipe, file->kind == FILE_KIND_PIPE_WRITE);
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
    if (file->kind == FILE_KIND_MEMFD && file->memfd)
        memfd_destroy(file->memfd);
    if (file->kind == FILE_KIND_SIGNALFD && file->signalfd)
        signalfd_destroy(file->signalfd);
    if (file->kind == FILE_KIND_SOCKET && file->socket)
        unix_socket_unref(file->socket);
    if (file->kind == FILE_KIND_INET_SOCKET && file->inet_socket)
        inet_socket_unref(file->inet_socket);
    if (file->kind == FILE_KIND_NETLINK_SOCKET && file->netlink_socket)
        netlink_socket_unref(file->netlink_socket);
    if ((file->kind == FILE_KIND_PTY_MASTER || file->kind == FILE_KIND_PTY_SLAVE) && file->pty)
        pty_close_endpoint(file->pty, file->kind == FILE_KIND_PTY_MASTER);
    kfree(file);
}

int64_t file_read(struct file *file, size_t size, void *buffer) {
    if (!file || !buffer) return -EBADF;
    if (file->kind == FILE_KIND_PIPE_READ) return pipe_read(file->pipe, size, buffer);
    if (file->kind == FILE_KIND_SOCKET) return unix_socket_read(file->socket, size, buffer);
    if (file->kind == FILE_KIND_INET_SOCKET) return inet_socket_read(file->inet_socket, size, buffer);
    if (file->kind == FILE_KIND_NETLINK_SOCKET) return netlink_socket_read(file->netlink_socket, size, buffer);
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
    if (file->kind == FILE_KIND_SIGNALFD)
        return signalfd_read(file->signalfd, size, buffer);
    /* A memfd carries a file offset like a regular file, so that reading it
       without mapping it behaves the way any other descriptor would. */
    if (file->kind == FILE_KIND_MEMFD) {
        int64_t moved = memfd_read(file->memfd, file->offset, size, buffer);
        if (moved > 0) file->offset += (uint64_t)moved;
        return moved;
    }
    if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    int64_t result = vfs_read(file->node, file->offset, size, buffer);
    if (result > 0) file->offset += (uint64_t)result;
    return result;
}

int64_t file_write(struct file *file, size_t size, const void *buffer) {
    if (!file || !buffer) return -EBADF;
    if (file->kind == FILE_KIND_SOCKET) return unix_socket_write(file->socket, size, buffer);
    if (file->kind == FILE_KIND_INET_SOCKET) return inet_socket_write(file->inet_socket, size, buffer);
    if (file->kind == FILE_KIND_NETLINK_SOCKET) return netlink_socket_write(file->netlink_socket, size, buffer);
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
    if (file->kind == FILE_KIND_MEMFD) {
        int64_t moved = memfd_write(file->memfd, file->offset, size, buffer);
        if (moved > 0) file->offset += (uint64_t)moved;
        return moved;
    }
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
    } else if (file->kind == FILE_KIND_NETLINK_SOCKET) {
        if (netlink_socket_read_ready(file->netlink_socket)) events |= pollin;
        if (netlink_socket_write_ready(file->netlink_socket)) events |= pollout;
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
    } else if (file->kind == FILE_KIND_SIGNALFD) {
        if (signalfd_read_ready(file->signalfd)) events |= pollin;
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
