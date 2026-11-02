#define TEST_NAME "ipv6"
#include "test.h"

#if defined(__x86_64__)
#define SYS_socket_nr   41
#define SYS_bind_nr     49
#define SYS_connect_nr  42
#define SYS_accept_nr   43
#define SYS_listen_nr   50
#define SYS_sendto_nr   44
#define SYS_recvfrom_nr 45
#define SYS_getsockname 51
#else
#define SYS_socket_nr   198
#define SYS_bind_nr     200
#define SYS_connect_nr  203
#define SYS_accept_nr   202
#define SYS_listen_nr   201
#define SYS_sendto_nr   206
#define SYS_recvfrom_nr 207
#define SYS_getsockname 204
#endif

#define AF_INET6     10
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define EAFNOSUPPORT 97

struct sockaddr_in6 {
    u16 family;
    u16 port;
    u32 flowinfo;
    u8 address[16];
    u32 scope_id;
};

static void loopback6(struct sockaddr_in6 *sa, u16 port) {
    u8 i;
    sa->family = AF_INET6;
    sa->port = (u16)((port >> 8) | (port << 8));
    sa->flowinfo = 0;
    for (i = 0; i < 16; i++) sa->address[i] = 0;
    sa->address[15] = 1;
    sa->scope_id = 0;
}

static int addr_is_loopback6(const u8 a[16]) {
    u8 i;
    for (i = 0; i < 15; i++)
        if (a[i]) return 0;
    return a[15] == 1;
}

static s64 udp_loopback(void) {
    int a = (int)SYSCALL(SYS_socket_nr, AF_INET6, SOCK_DGRAM, 0, 0, 0, 0);
    int b = (int)SYSCALL(SYS_socket_nr, AF_INET6, SOCK_DGRAM, 0, 0, 0, 0);
    if (a < 0 || b < 0) return 50;
    struct sockaddr_in6 sa;
    loopback6(&sa, 7777);
    if (SYSCALL(SYS_bind_nr, a, &sa, sizeof(sa), 0, 0, 0) != 0) return 51;
    struct sockaddr_in6 name;
    u64 nlen = sizeof(name);
    if (SYSCALL(SYS_getsockname, a, &name, &nlen, 0, 0, 0) != 0) return 52;
    if (name.family != AF_INET6 || !addr_is_loopback6(name.address)) return 53;
    if (SYSCALL(SYS_sendto_nr, b, "hello6", 6, 0, &sa, sizeof(sa)) != 6) return 54;
    char buffer[16];
    struct sockaddr_in6 from;
    u64 flen = sizeof(from);
    s64 got = -1;
    int tries;
    for (tries = 0; tries < 200 && got < 0; tries++) {
        got = SYSCALL(SYS_recvfrom_nr, a, buffer, sizeof(buffer), 0, &from, &flen);
        if (got < 0) sleep_ms(5);
    }
    if (got != 6) return 55;
    if (!memeq(buffer, "hello6", 6)) return 56;
    if (from.family != AF_INET6 || !addr_is_loopback6(from.address)) return 57;
    close(a);
    close(b);
    return 0;
}

static s64 tcp_loopback(void) {
    int srv = (int)SYSCALL(SYS_socket_nr, AF_INET6, SOCK_STREAM, 0, 0, 0, 0);
    if (srv < 0) return 60;
    struct sockaddr_in6 sa;
    loopback6(&sa, 7778);
    if (SYSCALL(SYS_bind_nr, srv, &sa, sizeof(sa), 0, 0, 0) != 0) return 61;
    if (SYSCALL(SYS_listen_nr, srv, 4, 0, 0, 0, 0) != 0) return 62;
    int cli = (int)SYSCALL(SYS_socket_nr, AF_INET6, SOCK_STREAM, 0, 0, 0, 0);
    if (cli < 0) return 63;
    if (SYSCALL(SYS_connect_nr, cli, &sa, sizeof(sa), 0, 0, 0) != 0) return 64;
    int conn = -1;
    int tries;
    for (tries = 0; tries < 200 && conn < 0; tries++) {
        conn = (int)SYSCALL(SYS_accept_nr, srv, 0, 0, 0, 0, 0);
        if (conn < 0) sleep_ms(5);
    }
    if (conn < 0) return 65;
    if (SYSCALL(SYS_sendto_nr, cli, "tcp6", 4, 0, 0, 0) != 4) return 66;
    char buffer[8];
    s64 got = -1;
    for (tries = 0; tries < 200 && got <= 0; tries++) {
        got = read(conn, buffer, sizeof(buffer));
        if (got <= 0) sleep_ms(5);
    }
    if (got != 4 || !memeq(buffer, "tcp6", 4)) return 67;
    close(conn);
    close(cli);
    close(srv);
    return 0;
}

static s64 mapped_rejected_unspecified(void) {
    int fd = (int)SYSCALL(SYS_socket_nr, AF_INET6, SOCK_DGRAM, 0, 0, 0, 0);
    if (fd < 0) return 70;
    struct sockaddr_in6 any;
    u8 i;
    any.family = AF_INET6;
    any.port = (u16)((7779 >> 8) | (7779 << 8));
    any.flowinfo = 0;
    for (i = 0; i < 16; i++) any.address[i] = 0;
    any.scope_id = 0;
    if (SYSCALL(SYS_bind_nr, fd, &any, sizeof(any), 0, 0, 0) != 0) return 71;
    close(fd);
    return 0;
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    expect_eq(udp_loopback(), 0, "an AF_INET6 UDP datagram crosses ::1 loopback");
    expect_eq(tcp_loopback(), 0, "an AF_INET6 TCP stream connects over ::1 loopback");
    expect_eq(mapped_rejected_unspecified(), 0, "AF_INET6 binds the unspecified address ::");
}
