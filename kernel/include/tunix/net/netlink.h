#ifndef TUNIX_NETLINK_H
#define TUNIX_NETLINK_H

#include <stddef.h>
#include <stdint.h>

#define TUNIX_AF_NETLINK             16
#define TUNIX_NETLINK_ROUTE          0
#define TUNIX_NETLINK_SOCK_DIAG      4
#define TUNIX_NETLINK_KOBJECT_UEVENT 15

#define TUNIX_UEVENT_GROUP_KERNEL 1
#define TUNIX_UEVENT_GROUP_UDEV   2

struct netlink_socket;

struct netlink_credentials {
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct tunix_sockaddr_nl {
    uint16_t family;
    uint16_t pad;
    uint32_t pid;
    uint32_t groups;
};

struct netlink_socket *netlink_socket_create(int protocol);
void netlink_socket_ref(struct netlink_socket *socket);
void netlink_socket_unref(struct netlink_socket *socket);

int netlink_socket_bind(struct netlink_socket *socket, const void *address, size_t length);
int netlink_socket_getsockname(struct netlink_socket *socket, void *address, size_t *length);

int64_t netlink_socket_sendto(struct netlink_socket *socket, const void *data, size_t length,
                              int flags, const void *address, size_t address_length);
int64_t netlink_socket_recvfrom(struct netlink_socket *socket, void *data, size_t length, int flags,
                                void *address, size_t *address_length);
int64_t netlink_socket_read(struct netlink_socket *socket, size_t length, void *data);
int64_t netlink_socket_write(struct netlink_socket *socket, size_t length, const void *data);

int netlink_socket_read_ready(struct netlink_socket *socket);
int netlink_socket_write_ready(struct netlink_socket *socket);

void netlink_socket_set_passcred(struct netlink_socket *socket, int on);
int netlink_socket_get_passcred(struct netlink_socket *socket);
void netlink_socket_last_credentials(struct netlink_socket *socket,
                                     struct netlink_credentials *out);

void netlink_uevent_broadcast(const void *message, size_t length);

#endif
