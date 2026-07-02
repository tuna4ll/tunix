#ifndef TUNIX_UNIX_SOCKET_H
#define TUNIX_UNIX_SOCKET_H

#include <stddef.h>
#include <stdint.h>

struct file;
struct unix_socket;

struct unix_credentials {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

#define TUNIX_AF_UNIX 1
#define TUNIX_SOCK_STREAM 1

struct tunix_sockaddr_un {
    uint16_t family;
    char path[108];
};

struct unix_socket *unix_socket_create(void);
void unix_socket_set_credentials(struct unix_socket *socket, int32_t pid, uint32_t uid, uint32_t gid);
int unix_socket_get_peer_credentials(struct unix_socket *socket, struct unix_credentials *credentials);
int unix_socket_get_name(struct unix_socket *socket, int peer,
                         struct tunix_sockaddr_un *address, size_t *length);
void unix_socket_set_passcred(struct unix_socket *socket, int enabled);
int unix_socket_get_passcred(struct unix_socket *socket);
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
int64_t unix_socket_send_with_rights(struct unix_socket *socket, size_t size,
                                     const void *buffer, struct file **files,
                                     size_t file_count);
int64_t unix_socket_recv_with_rights(struct unix_socket *socket, size_t size,
                                     void *buffer, struct file **files,
                                     size_t maximum_files, size_t *file_count);
int unix_socket_read_ready(struct unix_socket *socket);
int unix_socket_write_ready(struct unix_socket *socket);
int unix_socket_peer_closed(struct unix_socket *socket);
int unix_socket_is_listener(struct unix_socket *socket);
int unix_socket_shutdown(struct unix_socket *socket, int how);

#endif
