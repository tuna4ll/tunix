#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/heap.h"
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
    file->refs = 1;
    file->kind = FILE_KIND_VFS;
    file->flags = flags;
    file->offset = 0;
    file->node = node;
    file->pipe = NULL;
    file->socket = NULL;
    file->inet_socket = NULL;
    file->pty = NULL;
    if (node->open) node->open(node);
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
    if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    int64_t result = vfs_write(file->node, file->offset, size, buffer);
    if (result > 0) file->offset += (uint64_t)result;
    return result;
}
