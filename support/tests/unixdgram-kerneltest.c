typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_WRITE 1
#define SYS_CLOSE 3
#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_SENDTO 44
#define SYS_RECVFROM 45
#define SYS_BIND 49
#define SYS_EXIT_GROUP 231

#define AF_UNIX 1
#define SOCK_DGRAM 2
#define MSG_DONTWAIT 0x40
#define EAGAIN 11
#define ENOENT 2
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
    put("UNIXDGRAM ");
    put(name);
    put(ok ? " PASS" : " FAIL ");
    if (!ok) put_number(detail);
    put("\n");
    if (!ok) failures++;
}

static u64 address(struct sockaddr_un *out, const char *path) {
    out->family = AF_UNIX;
    u64 length = 0;
    while (path[length]) {
        out->path[length] = path[length];
        length++;
    }
    out->path[length] = 0;
    return 2 + length + 1;
}

static int same(const char *a, const char *b, s64 length) {
    for (s64 index = 0; index < length; index++)
        if (a[index] != b[index]) return 0;
    return 1;
}

void run(void) {
    struct sockaddr_un server_address, client_address, from;
    u64 server_length = address(&server_address, "/tmp/log.sock");
    u64 client_length = address(&client_address, "/tmp/c2.sock");

    int server = (int)syscall3(SYS_SOCKET, AF_UNIX, SOCK_DGRAM, 0);
    s64 status = syscall3(SYS_BIND, server, (s64)&server_address, (s64)server_length);
    check("bind", server >= 0 && status == 0, status);

    int anonymous = (int)syscall3(SYS_SOCKET, AF_UNIX, SOCK_DGRAM, 0);
    status = syscall6(SYS_SENDTO, anonymous, (s64)"one", 3, 0, (s64)&server_address,
                      (s64)server_length);
    check("sendto-unconnected", status == 3, status);

    int named = (int)syscall3(SYS_SOCKET, AF_UNIX, SOCK_DGRAM, 0);
    syscall3(SYS_BIND, named, (s64)&client_address, (s64)client_length);
    status = syscall3(SYS_CONNECT, named, (s64)&server_address, (s64)server_length);
    check("connect", status == 0, status);
    status = syscall3(SYS_WRITE, named, (s64)"second", 6);
    check("write-connected", status == 6, status);
    status = syscall3(SYS_WRITE, named, (s64)"3rd!!!!!!", 9);
    check("write-again", status == 9, status);

    char buffer[64];
    unsigned from_length = sizeof(from);
    status = syscall6(SYS_RECVFROM, server, (s64)buffer, sizeof(buffer), 0, (s64)&from,
                      (s64)&from_length);
    check("recv-first", status == 3 && same(buffer, "one", 3) && from_length == 2, status);
    from_length = sizeof(from);
    status = syscall6(SYS_RECVFROM, server, (s64)buffer, 4, 0, (s64)&from, (s64)&from_length);
    check("recv-truncates-one-message", status == 4 && same(buffer, "seco", 4), status);
    check("recv-names-sender", from_length == client_length && same(from.path, "/tmp/c2.sock", 13),
          from_length);
    status = syscall6(SYS_RECVFROM, server, (s64)buffer, sizeof(buffer), 0, 0, 0);
    check("recv-keeps-boundaries", status == 9 && same(buffer, "3rd!!!!!!", 9), status);
    status = syscall6(SYS_RECVFROM, server, (s64)buffer, sizeof(buffer), MSG_DONTWAIT, 0, 0);
    check("recv-empty", status == -EAGAIN, status);

    struct sockaddr_un missing;
    u64 missing_length = address(&missing, "/tmp/nothing.sock");
    status = syscall6(SYS_SENDTO, anonymous, (s64)"x", 1, 0, (s64)&missing, (s64)missing_length);
    check("sendto-missing", status == -ENOENT, status);

    syscall1(SYS_CLOSE, server);
    status = syscall3(SYS_WRITE, named, (s64)"late", 4);
    check("receiver-closed", status == -ECONNREFUSED, status);

    put(failures ? "UNIXDGRAM FAIL\n" : "UNIXDGRAM PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
