#ifndef TUNIX_NETLINK_H
#define TUNIX_NETLINK_H

#include <stddef.h>
#include <stdint.h>

/* Address family and protocols understood by the in-kernel netlink layer.
   Only what iproute2's ip/ss actually drive is implemented: NETLINK_ROUTE
   for rtnetlink dumps and NETLINK_SOCK_DIAG so ss can enumerate sockets. */
#define TUNIX_AF_NETLINK 16
#define TUNIX_NETLINK_ROUTE 0
#define TUNIX_NETLINK_SOCK_DIAG 4

struct netlink_socket;

/* struct sockaddr_nl as seen from userspace. */
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
int64_t netlink_socket_recvfrom(struct netlink_socket *socket, void *data, size_t length,
                                int flags, void *address, size_t *address_length);
int64_t netlink_socket_read(struct netlink_socket *socket, size_t length, void *data);
int64_t netlink_socket_write(struct netlink_socket *socket, size_t length, const void *data);

int netlink_socket_read_ready(struct netlink_socket *socket);
int netlink_socket_write_ready(struct netlink_socket *socket);

#endif
