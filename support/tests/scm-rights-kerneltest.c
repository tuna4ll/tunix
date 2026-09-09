typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_SENDMSG 46
#define SYS_RECVMSG 47
#define SYS_SOCKETPAIR 53
#define SYS_EXIT_GROUP 231

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_NONBLOCK 04000
#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define O_RDONLY 0

struct iovec {
    u64 base;
    u64 length;
};

struct msghdr {
    u64 name;
    u32 name_length;
    u32 padding;
    u64 iov;
    u64 iov_length;
    u64 control;
    u64 control_length;
    int flags;
    int padding2;
};

struct cmsghdr {
    u64 length;
    int level;
    int type;
};

struct control {
    struct cmsghdr header;
    int fd;
    int padding;
};

static inline s64 call1(s64 number, s64 first) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first)
                     : "rcx", "r11", "memory");
    return result;
}

static inline s64 call3(s64 number, s64 first, s64 second, s64 third) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first),
                     "S"(second), "d"(third) : "rcx", "r11", "memory");
    return result;
}

static inline s64 call4(s64 number, s64 first, s64 second, s64 third,
                        s64 fourth) {
    register s64 r10 __asm__("r10") = fourth;
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first),
                     "S"(second), "d"(third), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static u64 text_length(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    return length;
}

static void print(const char *text) {
    (void)call3(SYS_WRITE, 1, (s64)text, (s64)text_length(text));
}

static void zero(void *data, u64 size) {
    unsigned char *bytes = data;
    for (u64 index = 0; index < size; index++) bytes[index] = 0;
}

static int socket_pair(int sockets[2]) {
    return call4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0,
                 (s64)sockets) == 0;
}

static int send_right(int socket, int fd, char byte) {
    struct iovec iov = {(u64)&byte, 1};
    struct control control;
    zero(&control, sizeof(control));
    control.header.length = sizeof(struct cmsghdr) + sizeof(int);
    control.header.level = SOL_SOCKET;
    control.header.type = SCM_RIGHTS;
    control.fd = fd;
    struct msghdr message;
    zero(&message, sizeof(message));
    message.iov = (u64)&iov;
    message.iov_length = 1;
    message.control = (u64)&control;
    message.control_length = sizeof(control);
    return call3(SYS_SENDMSG, socket, (s64)&message, 0) == 1;
}

static s64 receive(int socket, char *data, u64 capacity, int *fd_count,
                   int *received_fd) {
    struct iovec iov = {(u64)data, capacity};
    unsigned char control[64];
    zero(control, sizeof(control));
    struct msghdr message;
    zero(&message, sizeof(message));
    message.iov = (u64)&iov;
    message.iov_length = 1;
    message.control = (u64)control;
    message.control_length = sizeof(control);
    s64 result = call3(SYS_RECVMSG, socket, (s64)&message, 0);
    *fd_count = 0;
    *received_fd = -1;
    if (result < 0 || message.control_length < sizeof(struct cmsghdr)) return result;
    struct cmsghdr *header = (struct cmsghdr *)control;
    if (header->level != SOL_SOCKET || header->type != SCM_RIGHTS ||
        header->length < sizeof(*header)) return result;
    *fd_count = (int)((header->length - sizeof(*header)) / sizeof(int));
    if (*fd_count > 0) *received_fd = *(int *)(control + sizeof(*header));
    return result;
}

static void close_pair(int sockets[2]) {
    (void)call1(SYS_CLOSE, sockets[0]);
    (void)call1(SYS_CLOSE, sockets[1]);
}

static int test_boundaries(int first_fd, int second_fd) {
    int sockets[2];
    if (!socket_pair(sockets)) return 0;
    if (!send_right(sockets[0], first_fd, 'A') ||
        !send_right(sockets[0], second_fd, 'B')) return 0;
    char data[8];
    int count;
    int received;
    s64 length = receive(sockets[1], data, sizeof(data), &count, &received);
    if (length != 1 || data[0] != 'A' || count != 1 || received < 0) return 0;
    (void)call1(SYS_CLOSE, received);
    length = receive(sockets[1], data, sizeof(data), &count, &received);
    if (length != 1 || data[0] != 'B' || count != 1 || received < 0) return 0;
    (void)call1(SYS_CLOSE, received);
    close_pair(sockets);
    return 1;
}

static int test_offset(int fd) {
    int sockets[2];
    if (!socket_pair(sockets)) return 0;
    if (call3(SYS_WRITE, sockets[0], (s64)"xyz", 3) != 3 ||
        !send_right(sockets[0], fd, 'R')) return 0;
    char data[8];
    int count;
    int received;
    s64 length = receive(sockets[1], data, 2, &count, &received);
    if (length != 2 || data[0] != 'x' || data[1] != 'y' || count != 0) return 0;
    length = receive(sockets[1], data, sizeof(data), &count, &received);
    if (length != 2 || data[0] != 'z' || data[1] != 'R' ||
        count != 1 || received < 0) return 0;
    (void)call1(SYS_CLOSE, received);
    close_pair(sockets);
    return 1;
}

static int test_plain_read_discards(int fd) {
    int sockets[2];
    if (!socket_pair(sockets) || !send_right(sockets[0], fd, 'Q')) return 0;
    char byte;
    if (call3(SYS_READ, sockets[1], (s64)&byte, 1) != 1 || byte != 'Q') return 0;
    if (call3(SYS_WRITE, sockets[0], (s64)"Z", 1) != 1) return 0;
    int count;
    int received;
    if (receive(sockets[1], &byte, 1, &count, &received) != 1 ||
        byte != 'Z' || count != 0) return 0;
    close_pair(sockets);
    return 1;
}

static int run_test(void) {
    int first_fd = (int)call3(SYS_OPEN, (s64)"/proc/abi_gaps", O_RDONLY, 0);
    int second_fd = (int)call3(SYS_OPEN, (s64)"/proc/abi_gaps", O_RDONLY, 0);
    if (first_fd < 0 || second_fd < 0) return 0;
    int passed = test_boundaries(first_fd, second_fd) &&
                 test_offset(first_fd) &&
                 test_plain_read_discards(first_fd);
    (void)call1(SYS_CLOSE, first_fd);
    (void)call1(SYS_CLOSE, second_fd);
    return passed;
}

void _start(void) {
    if (run_test()) print("SCMRIGHTSTEST PASS\n");
    else print("SCMRIGHTSTEST FAIL\n");
    call1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}
