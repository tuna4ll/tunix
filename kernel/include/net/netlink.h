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
/* Device notifications, which is how udevd hears that a device exists at all:
   writing an action to a /sys uevent file broadcasts it on this family, udevd
   applies its rules and re-broadcasts the result, and its listeners -- weston's
   display and input layers among them -- act on that. */
#define TUNIX_NETLINK_KOBJECT_UEVENT 15

/* The two multicast groups on that family, numbered as Linux numbers them: the
   kernel announces on 1, udevd re-announces on 2. A bind asks for them as a
   mask, so group N is bit N-1. */
#define TUNIX_UEVENT_GROUP_KERNEL 1
#define TUNIX_UEVENT_GROUP_UDEV 2

struct netlink_socket;

/* Who sent the datagram a read just returned. udev refuses any message whose
   sender is not root, so this has to travel with the data. */
struct netlink_credentials {
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
};

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

/* SO_PASSCRED, which udev's monitor sets before it will believe anything. */
void netlink_socket_set_passcred(struct netlink_socket *socket, int on);
int netlink_socket_get_passcred(struct netlink_socket *socket);
/* The credentials that came with the datagram the last read handed over. */
void netlink_socket_last_credentials(struct netlink_socket *socket,
                                     struct netlink_credentials *out);

/* Announce a device event to everything listening on the kernel group. The
   message is Linux's: a "<action>@<devpath>" line, then NUL-terminated
   KEY=VALUE properties. */
void netlink_uevent_broadcast(const void *message, size_t length);

#endif
