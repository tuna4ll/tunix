#ifndef TUNIX_UNIX_SOCKET_H
#define TUNIX_UNIX_SOCKET_H

#include <stddef.h>
#include <stdint.h>

struct file;
struct unix_socket;

#define TUNIX_AF_UNIX 1
#define TUNIX_SOCK_STREAM 1

struct tunix_sockaddr_un {
    uint16_t family;
    char path[108];
};

struct unix_socket *unix_socket_create(void);
int unix_socket_pair(struct unix_socket **first, struct unix_socket **second);
void unix_socket_ref(struct unix_socket *socket);
void unix_socket_unref(struct unix_socket *socket);
int unix_socket_bind(struct unix_socket *socket, const struct tunix_sockaddr_un *address,
                     size_t length);
int unix_socket_listen(struct unix_socket *socket, int backlog);
int unix_socket_connect(struct unix_socket *socket, const struct tunix_sockaddr_un *address,
                        size_t length);
struct unix_socket *unix_socket_accept(struct unix_socket *socket);
int64_t unix_socket_read(struct unix_socket *socket, size_t size, void *buffer);
int64_t unix_socket_write(struct unix_socket *socket, size_t size, const void *buffer);
int unix_socket_read_ready(struct unix_socket *socket);
int unix_socket_write_ready(struct unix_socket *socket);
int unix_socket_is_listener(struct unix_socket *socket);

#endif
