typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_CLOSE 3
#define SYS_POLL 7
#define SYS_PIPE 22
#define SYS_NANOSLEEP 35
#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_BIND 49
#define SYS_LISTEN 50
#define SYS_FORK 57
#define SYS_WAIT4 61
#define SYS_EXIT_GROUP 231

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_NONBLOCK 04000
#define EAGAIN 11
#define ECONNREFUSED 111

struct sockaddr_un {
    unsigned short family;
    char path[108];
};

static unsigned failures;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)text, (s64)length);
}

static void put_number(s64 value) {
    char digits[24];
    int count = 0;
    u64 magnitude = value < 0 ? (u64)-value : (u64)value;
    do {
        digits[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    if (value < 0) put("-");
    char out[24];
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
    put(out);
}

static void check(const char *name, int ok, s64 detail) {
    put("UNIXLISTEN ");
    put(name);
    put(ok ? " PASS" : " FAIL ");
    if (!ok) put_number(detail);
    put("\n");
    if (!ok) failures++;
}

static u64 address(struct sockaddr_un *out, const char *path, int abstract) {
    out->family = AF_UNIX;
    u64 length = 0;
    if (abstract) out->path[length++] = 0;
    for (u64 index = 0; path[index]; index++) out->path[length++] = path[index];
    if (abstract) return 2 + length;
    out->path[length] = 0;
    return 2 + length + 1;
}

static int listener(struct sockaddr_un *where, u64 length, int backlog) {
    int fd = (int)syscall3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return fd;
    s64 status = syscall3(SYS_BIND, fd, (s64)where, (s64)length);
    if (status == 0) status = syscall2(SYS_LISTEN, fd, backlog);
    return status == 0 ? fd : (int)status;
}

static s64 dial(struct sockaddr_un *where, u64 length, int flags) {
    int fd = (int)syscall3(SYS_SOCKET, AF_UNIX, SOCK_STREAM | flags, 0);
    if (fd < 0) return fd;
    s64 status = syscall3(SYS_CONNECT, fd, (s64)where, (s64)length);
    return status == 0 ? fd : status;
}

static void nap_ms(s64 milliseconds) {
    struct { s64 seconds, nanoseconds; } request = { 0, milliseconds * 1000000 };
    (void)syscall2(SYS_NANOSLEEP, (s64)&request, 0);
}

static void backlog_one(int abstract) {
    struct sockaddr_un where;
    u64 length = address(&where, abstract ? "/tmp/.X11-unix/X9" : "/tmp/one.sock", abstract);
    const char *tag = abstract ? "abstract-" : "path-";
    int server = listener(&where, length, 1);
    char name[64];
    const char *parts[] = { tag, "listen", 0 };
    u64 at = 0;
    for (int part = 0; parts[part]; part++)
        for (const char *c = parts[part]; *c; c++) name[at++] = *c;
    name[at] = 0;
    check(name, server >= 0, server);

    s64 first = dial(&where, length, 0);
    s64 second = dial(&where, length, 0);
    check(abstract ? "abstract-backlog-plus-one" : "path-backlog-plus-one", first >= 0 && second >= 0,
          first < 0 ? first : second);
    s64 third = dial(&where, length, SOCK_NONBLOCK);
    check(abstract ? "abstract-full-nonblocking" : "path-full-nonblocking", third == -EAGAIN, third);

    int channel[2];
    (void)syscall1(SYS_PIPE, (s64)channel);
    s64 child = syscall1(SYS_FORK, 0);
    if (child == 0) {
        s64 waited = dial(&where, length, 0);
        s64 result = waited >= 0 ? 0 : waited;
        (void)syscall3(SYS_WRITE, channel[1], (s64)&result, sizeof(result));
        (void)syscall1(SYS_EXIT_GROUP, 0);
    }
    struct { int fd; short events, revents; } watch = { channel[0], 1, 0 };
    s64 early = syscall3(SYS_POLL, (s64)&watch, 1, 200);
    check(abstract ? "abstract-full-blocking-waits" : "path-full-blocking-waits", early == 0, early);

    s64 accepted = syscall3(SYS_ACCEPT, server, 0, 0);
    watch.revents = 0;
    s64 late = syscall3(SYS_POLL, (s64)&watch, 1, 2000);
    s64 result = -1;
    if (late == 1) (void)syscall3(SYS_READ, channel[0], (s64)&result, sizeof(result));
    check(abstract ? "abstract-accept-releases-waiter" : "path-accept-releases-waiter",
          accepted >= 0 && late == 1 && result == 0, late == 1 ? result : late);
    (void)syscall4(SYS_WAIT4, child, 0, 0, 0);

    char byte = 'x';
    (void)syscall3(SYS_WRITE, (int)first, (s64)&byte, 1);
    char got = 0;
    s64 read = syscall3(SYS_READ, (int)accepted, (s64)&got, 1);
    check(abstract ? "abstract-first-is-accepted" : "path-first-is-accepted", read == 1 && got == 'x', read);

    (void)syscall1(SYS_CLOSE, channel[0]);
    (void)syscall1(SYS_CLOSE, channel[1]);
    (void)syscall1(SYS_CLOSE, (int)first);
    (void)syscall1(SYS_CLOSE, (int)second);
    (void)syscall1(SYS_CLOSE, (int)accepted);
    (void)syscall1(SYS_CLOSE, server);
    nap_ms(10);
}

static void backlog_zero(void) {
    struct sockaddr_un where;
    u64 length = address(&where, "/tmp/zero.sock", 0);
    int server = listener(&where, length, 0);
    s64 first = dial(&where, length, SOCK_NONBLOCK);
    s64 second = dial(&where, length, SOCK_NONBLOCK);
    check("zero-backlog-takes-one", server >= 0 && first >= 0 && second == -EAGAIN,
          first < 0 ? first : second);
    struct sockaddr_un nowhere;
    u64 nowhere_length = address(&nowhere, "/tmp/none.sock", 0);
    s64 refused = dial(&nowhere, nowhere_length, 0);
    check("no-listener-refused", refused == -ECONNREFUSED || refused == -2, refused);
}

void run(void) {
    backlog_one(0);
    backlog_one(1);
    backlog_zero();
    put(failures ? "UNIXLISTEN FAIL\n" : "UNIXLISTEN PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
