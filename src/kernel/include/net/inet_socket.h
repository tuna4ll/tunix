#ifndef TUNIX_INET_SOCKET_H
#define TUNIX_INET_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#define TUNIX_AF_INET 2
#define TUNIX_AF_PACKET 17
#define TUNIX_SOCK_STREAM 1
#define TUNIX_SOCK_DGRAM 2
#define TUNIX_SOCK_RAW 3
#define TUNIX_SOCK_PACKET 10

struct inet_socket;

struct tunix_sockaddr {
    uint16_t family;
    char data[14];
};

struct tunix_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};

struct tunix_sockaddr_ll {
    uint16_t family;
    uint16_t protocol;
    int32_t ifindex;
    uint16_t hatype;
    uint8_t packet_type;
    uint8_t halen;
    uint8_t address[8];
};

struct inet_socket *inet_socket_create(int domain, int type, int protocol);
void inet_socket_ref(struct inet_socket *socket);
void inet_socket_unref(struct inet_socket *socket);
int inet_socket_bind(struct inet_socket *socket, const void *address, size_t length);
int inet_socket_connect(struct inet_socket *socket, const void *address, size_t length);
int64_t inet_socket_sendto(struct inet_socket *socket, const void *data, size_t length, int flags,
                           const void *address, size_t address_length);
int64_t inet_socket_recvfrom(struct inet_socket *socket, void *data, size_t length, int flags,
                             void *address, size_t *address_length);
int inet_socket_getsockname(struct inet_socket *socket, void *address, size_t *length);
int inet_socket_getpeername(struct inet_socket *socket, void *address, size_t *length);
int inet_socket_setsockopt(struct inet_socket *socket, int level, int option,
                           const void *value, size_t length);
int inet_socket_getsockopt(struct inet_socket *socket, int level, int option,
                           void *value, size_t *length);
int inet_socket_ioctl(struct inet_socket *socket, unsigned long request, void *argument);
int inet_socket_read_ready(struct inet_socket *socket);
int inet_socket_write_ready(struct inet_socket *socket);
int inet_socket_shutdown(struct inet_socket *socket, int how);
int64_t inet_socket_read(struct inet_socket *socket, size_t length, void *data);
int64_t inet_socket_write(struct inet_socket *socket, size_t length, const void *data);
void inet_socket_receive_ipv4(const uint8_t *packet, size_t length, uint8_t protocol,
                              uint32_t source, uint32_t destination);
void inet_socket_receive_udp(const uint8_t *payload, size_t length, uint32_t source,
                             uint16_t source_port, uint32_t destination, uint16_t destination_port);
void inet_socket_receive_ethernet(const uint8_t *frame, size_t length, uint16_t ethertype);
void inet_socket_receive_tcp(uint32_t source, uint16_t source_port, uint32_t destination,
                             uint16_t destination_port, uint32_t seq, uint32_t ack, uint8_t flags,
                             uint16_t window, const uint8_t *payload, size_t length);
void inet_socket_tcp_timer_poll(void);
int inet_socket_peer_closed(struct inet_socket *socket);
void inet_socket_proc_udp(char *buffer, size_t capacity, size_t *length);
void inet_socket_proc_raw(char *buffer, size_t capacity, size_t *length);
void inet_socket_proc_tcp(char *buffer, size_t capacity, size_t *length);

#endif
