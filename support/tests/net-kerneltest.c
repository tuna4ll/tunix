typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#if defined(__x86_64__)
#define NR_READ 0
#define NR_WRITE 1
#define NR_CLOSE 3
#define NR_IOCTL 16
#define NR_SOCKET 41
#define NR_CONNECT 42
#define NR_SENDTO 44
#define NR_RECVFROM 45
#define NR_BIND 49
#define NR_LISTEN 50
#define NR_GETSOCKNAME 51
#define NR_SETSOCKOPT 54
#define NR_GETSOCKOPT 55
#define NR_FORK 57
#define NR_EXIT 60
#define NR_WAIT4 61
#define NR_CLOCK_GETTIME 228
#define NR_EXIT_GROUP 231
#define NR_ACCEPT4 288
#define NR_PRLIMIT64 302
#define NR_OPENAT 257

static inline s64 call6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
    s64 r;
    register s64 r10 __asm__("r10") = d;
    register s64 r8 __asm__("r8") = e;
    register s64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
static s64 do_fork(void) { return call6(NR_FORK, 0, 0, 0, 0, 0, 0); }
#elif defined(__aarch64__)
#define NR_READ 63
#define NR_WRITE 64
#define NR_CLOSE 57
#define NR_IOCTL 29
#define NR_SOCKET 198
#define NR_CONNECT 203
#define NR_SENDTO 206
#define NR_RECVFROM 207
#define NR_BIND 200
#define NR_LISTEN 201
#define NR_GETSOCKNAME 204
#define NR_SETSOCKOPT 208
#define NR_GETSOCKOPT 209
#define NR_CLONE 220
#define NR_EXIT 93
#define NR_WAIT4 260
#define NR_CLOCK_GETTIME 113
#define NR_EXIT_GROUP 94
#define NR_ACCEPT4 242
#define NR_PRLIMIT64 261
#define NR_OPENAT 56

static inline s64 call6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
    register s64 x8 __asm__("x8") = n;
    register s64 x0 __asm__("x0") = a;
    register s64 x1 __asm__("x1") = b;
    register s64 x2 __asm__("x2") = c;
    register s64 x3 __asm__("x3") = d;
    register s64 x4 __asm__("x4") = e;
    register s64 x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}
static s64 do_fork(void) { return call6(NR_CLONE, 17, 0, 0, 0, 0, 0); }
#endif

#define call1(n, a) call6(n, (s64)(a), 0, 0, 0, 0, 0)
#define call2(n, a, b) call6(n, (s64)(a), (s64)(b), 0, 0, 0, 0)
#define call3(n, a, b, c) call6(n, (s64)(a), (s64)(b), (s64)(c), 0, 0, 0)
#define call4(n, a, b, c, d) call6(n, (s64)(a), (s64)(b), (s64)(c), (s64)(d), 0, 0)
#define call5(n, a, b, c, d, e) call6(n, (s64)(a), (s64)(b), (s64)(c), (s64)(d), (s64)(e), 0)

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_RCVBUF 8
#define RLIMIT_NOFILE 7
#define SIOCSIFADDR 0x8916
#define SIOCSIFNETMASK 0x891C
#define SIOCADDRT 0x890B
#define SIOCGIFFLAGS 0x8913
#define IFF_RUNNING 0x40

#ifndef HOST_PORT
#define HOST_PORT 0
#endif

struct sockaddr_in {
    u16 family;
    u16 port;
    u32 address;
    u8 zero[8];
};

static int failures;

static u64 length_of(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    return length;
}

static void print(const char *text) { call3(NR_WRITE, 1, text, length_of(text)); }

static void print_number(u64 value) {
    char digits[24];
    int at = 23;
    digits[at] = 0;
    do {
        digits[--at] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(digits + at);
}

static void report(const char *name, int ok, s64 value) {
    print(ok ? "NET ok " : "NET FAIL ");
    print(name);
    print(" ");
    if (value < 0) {
        print("-");
        value = -value;
    }
    print_number((u64)value);
    print("\n");
    if (!ok) failures++;
}

static u16 swap16(u16 value) { return (u16)((value << 8) | (value >> 8)); }

static struct sockaddr_in address_of(u8 a, u8 b, u8 c, u8 d, u16 port) {
    struct sockaddr_in in;
    for (int i = 0; i < 8; i++) in.zero[i] = 0;
    in.family = AF_INET;
    in.port = swap16(port);
    in.address = (u32)a | ((u32)b << 8) | ((u32)c << 16) | ((u32)d << 24);
    return in;
}

static u64 now_ms(void) {
    s64 spec[2];
    call2(NR_CLOCK_GETTIME, 1, spec);
    return (u64)spec[0] * 1000 + (u64)spec[1] / 1000000;
}

static u8 byte_at(u64 index) { return (u8)(index * 7 + index / 251); }

static char chunk[65536];
static char incoming[65536];

static s64 write_full(s64 fd, const char *data, u64 count) {
    u64 sent = 0;
    while (sent < count) {
        s64 n = call3(NR_WRITE, fd, data + sent, count - sent);
        if (n <= 0) return n;
        sent += (u64)n;
    }
    return (s64)sent;
}

static void loopback_bulk(void) {
    const u64 total = 32ULL << 20;
    s64 listener = call3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in local = address_of(127, 0, 0, 1, 5001);
    call3(NR_BIND, listener, &local, sizeof(local));
    call2(NR_LISTEN, listener, 8);
    s64 child = do_fork();
    if (child == 0) {
        s64 fd = call3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
        if (call3(NR_CONNECT, fd, &local, sizeof(local)) != 0) call1(NR_EXIT, 1);
        for (u64 sent = 0; sent < total; sent += sizeof(chunk)) {
            for (u64 i = 0; i < sizeof(chunk); i++) chunk[i] = (char)byte_at(sent + i);
            if (write_full(fd, chunk, sizeof(chunk)) != (s64)sizeof(chunk)) call1(NR_EXIT, 2);
        }
        call1(NR_CLOSE, fd);
        call1(NR_EXIT, 0);
    }
    u64 started = now_ms();
    s64 fd = call4(NR_ACCEPT4, listener, 0, 0, 0);
    u64 received = 0;
    int intact = fd >= 0;
    while (intact) {
        s64 n = call3(NR_READ, fd, incoming, sizeof(incoming));
        if (n <= 0) break;
        for (s64 i = 0; i < n; i++)
            if ((u8)incoming[i] != byte_at(received + (u64)i)) intact = 0;
        received += (u64)n;
    }
    u64 elapsed = now_ms() - started;
    int status = 0;
    call4(NR_WAIT4, child, &status, 0, 0);
    report("loopback-tcp-32mib", intact && received == total && status == 0, (s64)received);
    report("loopback-tcp-mib-per-second", 1, elapsed ? (s64)(32 * 1000 / elapsed) : 0);
    call1(NR_CLOSE, fd);
    call1(NR_CLOSE, listener);
}

static void loopback_datagrams(void) {
    s64 fd = call3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = address_of(127, 0, 0, 1, 5002);
    call3(NR_BIND, fd, &local, sizeof(local));
    for (u64 i = 0; i < 60000; i++) chunk[i] = (char)byte_at(i);
    s64 sent = call6(NR_SENDTO, fd, (s64)chunk, 60000, 0, (s64)&local, sizeof(local));
    s64 got = call6(NR_RECVFROM, fd, (s64)incoming, sizeof(incoming), 0, 0, 0);
    int same = got == 60000;
    for (s64 i = 0; same && i < got; i++) if (incoming[i] != chunk[i]) same = 0;
    report("loopback-udp-60000-byte-datagram", sent == 60000 && same, got);

    for (int i = 0; i < 100; i++) {
        chunk[0] = (char)i;
        call6(NR_SENDTO, fd, (s64)chunk, 1000, 0, (s64)&local, sizeof(local));
    }
    int ordered = 1, count = 0;
    for (int i = 0; i < 100; i++) {
        got = call6(NR_RECVFROM, fd, (s64)incoming, sizeof(incoming), 0x40, 0, 0);
        if (got != 1000) break;
        if (incoming[0] != (char)i) ordered = 0;
        count++;
    }
    report("loopback-udp-100-queued", count == 100 && ordered, count);

    int wanted = 1 << 20, size = 0;
    u32 size_length = sizeof(size);
    call5(NR_SETSOCKOPT, fd, SOL_SOCKET, SO_RCVBUF, &wanted, sizeof(wanted));
    call5(NR_GETSOCKOPT, fd, SOL_SOCKET, SO_RCVBUF, &size, &size_length);
    report("so-rcvbuf-doubled", size == 2 * wanted, size);
    call1(NR_CLOSE, fd);
}

#define PAIRS 3000

static s64 clients[PAIRS];
static s64 servers[PAIRS];

static void many_connections(void) {
    u64 limit[2] = {20000, 20000};
    call4(NR_PRLIMIT64, 0, RLIMIT_NOFILE, limit, 0);
    s64 listener = call3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in local = address_of(127, 0, 0, 1, 5003);
    call3(NR_BIND, listener, &local, sizeof(local));
    call2(NR_LISTEN, listener, 4096);
    int made = 0;
    for (; made < PAIRS; made++) {
        clients[made] = call3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
        if (clients[made] < 0 ||
            call3(NR_CONNECT, clients[made], &local, sizeof(local)) != 0) break;
        servers[made] = call4(NR_ACCEPT4, listener, 0, 0, 0);
        if (servers[made] < 0) break;
    }
    report("loopback-connections", made == PAIRS, made);
    int echoed = 0;
    for (int i = 0; i < made; i++) {
        char value = (char)i, back = 0;
        call3(NR_WRITE, clients[i], &value, 1);
        if (call3(NR_READ, servers[i], &back, 1) == 1 && back == value) echoed++;
    }
    report("loopback-connections-distinct", echoed == made, echoed);
    s64 table = call4(NR_OPENAT, -100, "/proc/net/tcp", 0, 0);
    s64 lines = 0;
    for (;;) {
        s64 n = call3(NR_READ, table, incoming, sizeof(incoming));
        if (n <= 0) break;
        for (s64 i = 0; i < n; i++) if (incoming[i] == '\n') lines++;
    }
    call1(NR_CLOSE, table);
    report("proc-net-tcp-lists-every-connection", lines > 2 * made, lines);
    for (int i = 0; i < made; i++) {
        call1(NR_CLOSE, clients[i]);
        call1(NR_CLOSE, servers[i]);
    }
    call1(NR_CLOSE, listener);
}

static void external(void) {
    s64 probe = call3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    struct { char name[16]; struct sockaddr_in address; } request;
    for (u64 i = 0; i < sizeof(request); i++) ((char *)&request)[i] = 0;
    request.name[0] = 'e'; request.name[1] = 't'; request.name[2] = 'h'; request.name[3] = '0';
    call3(NR_IOCTL, probe, SIOCGIFFLAGS, &request);
    if (!(*(u16 *)&request.address & IFF_RUNNING)) {
        report("external-skipped-no-adapter", 1, 0);
        call1(NR_CLOSE, probe);
        return;
    }
    request.address = address_of(10, 0, 2, 15, 0);
    call3(NR_IOCTL, probe, SIOCSIFADDR, &request);
    request.address = address_of(255, 255, 255, 0, 0);
    call3(NR_IOCTL, probe, SIOCSIFNETMASK, &request);
    struct { u64 pad; struct sockaddr_in destination; struct sockaddr_in gateway; char rest[64]; } route;
    for (u64 i = 0; i < sizeof(route); i++) ((char *)&route)[i] = 0;
    route.gateway = address_of(10, 0, 2, 2, 0);
    call3(NR_IOCTL, probe, SIOCADDRT, &route);
    call1(NR_CLOSE, probe);

    struct sockaddr_in host = address_of(10, 0, 2, 2, HOST_PORT);
    s64 fd = call3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    for (u64 i = 0; i < 8000; i++) chunk[i] = (char)byte_at(i + 3);
    s64 got = -1;
    for (int attempt = 0; attempt < 20 && got != 8000; attempt++) {
        call6(NR_SENDTO, fd, (s64)chunk, 8000, 0, (s64)&host, sizeof(host));
        u64 deadline = now_ms() + 500;
        do got = call6(NR_RECVFROM, fd, (s64)incoming, sizeof(incoming), 0x40, 0, 0);
        while (got < 0 && now_ms() < deadline);
    }
    int same = got == 8000;
    for (s64 i = 0; same && i < got; i++) if (incoming[i] != chunk[i]) same = 0;
    report("external-udp-fragmented-echo", same, got);
    call1(NR_CLOSE, fd);

    const u64 total = 4ULL << 20;
    fd = call3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    s64 status = call3(NR_CONNECT, fd, &host, sizeof(host));
    report("external-tcp-connect", status == 0, status);
    if (status != 0) return;
    s64 child = do_fork();
    if (child == 0) {
        for (u64 sent = 0; sent < total; sent += sizeof(chunk)) {
            for (u64 i = 0; i < sizeof(chunk); i++) chunk[i] = (char)byte_at(sent + i);
            if (write_full(fd, chunk, sizeof(chunk)) != (s64)sizeof(chunk)) call1(NR_EXIT, 2);
        }
        call1(NR_EXIT, 0);
    }
    u64 started = now_ms();
    u64 received = 0;
    int intact = 1;
    while (received < total) {
        s64 n = call3(NR_READ, fd, incoming, sizeof(incoming));
        if (n <= 0) break;
        for (s64 i = 0; i < n; i++)
            if ((u8)incoming[i] != byte_at(received + (u64)i)) intact = 0;
        received += (u64)n;
    }
    u64 elapsed = now_ms() - started;
    int child_status = 0;
    call4(NR_WAIT4, child, &child_status, 0, 0);
    report("external-tcp-4mib-echo", intact && received == total && child_status == 0,
           (s64)received);
    report("external-tcp-kib-per-second", 1, elapsed ? (s64)(4096 * 1000 / elapsed) : 0);
    call1(NR_CLOSE, fd);
}

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    loopback_bulk();
    loopback_datagrams();
    many_connections();
    if (HOST_PORT) external();
    print(failures ? "NETTEST FAIL\n" : "NETTEST PASS\n");
    call1(NR_EXIT_GROUP, 0);
    for (;;) { }
}

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call run\n"
        "    hlt\n");
#else
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov x29, #0\n"
        "    bl run\n"
        "    b .\n");
#endif
