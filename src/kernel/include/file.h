#ifndef TUNIX_FILE_H
#define TUNIX_FILE_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct pipe_buffer;
struct unix_socket;
struct inet_socket;
struct pty_pair;

#define FILE_KIND_VFS        1
#define FILE_KIND_PIPE_READ  2
#define FILE_KIND_PIPE_WRITE 3
#define FILE_KIND_SOCKET     4
#define FILE_KIND_PTY_MASTER 5
#define FILE_KIND_PTY_SLAVE  6
#define FILE_KIND_INET_SOCKET 7

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
};

struct file *file_open_node(struct vfs_node *node, uint32_t flags);
struct file *file_create_pipe_end(struct pipe_buffer *pipe, int write_end);
struct file *file_create_socket(struct unix_socket *socket);
struct file *file_create_inet_socket(struct inet_socket *socket);
struct file *file_create_pty_endpoint(struct pty_pair *pty, int master,
                                      struct vfs_node *node, uint32_t flags);
void file_ref(struct file *file);
void file_unref(struct file *file);
int64_t file_read(struct file *file, size_t size, void *buffer);
int64_t file_write(struct file *file, size_t size, const void *buffer);

#endif
