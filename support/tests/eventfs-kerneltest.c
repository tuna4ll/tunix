typedef unsigned long u64;
typedef long s64;

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_fstat 5
#define SYS_poll 7
#define SYS_nanosleep 35
#define SYS_socket 41
#define SYS_connect 42
#define SYS_accept 43
#define SYS_bind 49
#define SYS_listen 50
#define SYS_fork 57
#define SYS_wait4 61
#define SYS_rename 82
#define SYS_unlink 87
#define SYS_exit_group 231

#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_NONBLOCK 04000
#define POLLIN 1
#define EAGAIN 11
#define EMSGSIZE 90
#define AF_INET 2
#define SOCK_STREAM 1

static inline s64 call1(s64 number, s64 first) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first)
                     : "rcx", "r11", "memory");
    return result;
}

static inline s64 call2(s64 number, s64 first, s64 second) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first),
                     "S"(second) : "rcx", "r11", "memory");
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
    (void)call3(SYS_write, 1, (s64)text, (s64)text_length(text));
}

static int contains(const char *data, s64 length, const char *needle) {
    u64 wanted = text_length(needle);
    if (length < 0 || (u64)length < wanted) return 0;
    for (u64 at = 0; at + wanted <= (u64)length; at++) {
        u64 index = 0;
        while (index < wanted && data[at + index] == needle[index]) index++;
        if (index == wanted) return 1;
    }
    return 0;
}

static void sleep_ms(u64 milliseconds) {
    struct { s64 seconds, nanoseconds; } request;
    request.seconds = (s64)(milliseconds / 1000U);
    request.nanoseconds = (s64)((milliseconds % 1000U) * 1000000U);
    (void)call2(SYS_nanosleep, (s64)&request, 0);
}

static int process_stream_test(void) {
    int first = (int)call3(SYS_open, (s64)"/events/process", O_NONBLOCK, 0);
    int second = (int)call3(SYS_open, (s64)"/events/process", O_NONBLOCK, 0);
    if (first < 0 || second < 0) return 0;
    u64 stat_buffer[32];
    if (call2(SYS_fstat, first, (s64)stat_buffer) != 0) return 0;

    s64 child = call1(SYS_fork, 0);
    if (child == 0) {
        sleep_ms(200);
        (void)call1(SYS_exit_group, 7);
        for (;;) { }
    }
    if (child < 0) return 0;

    char tiny[1];
    if (call3(SYS_read, first, (s64)tiny, sizeof(tiny)) != -EMSGSIZE) return 0;
    char left[512], right[512];
    s64 left_size = call3(SYS_read, first, (s64)left, sizeof(left));
    s64 right_size = call3(SYS_read, second, (s64)right, sizeof(right));
    if (!contains(left, left_size, "fork ") || !contains(right, right_size, "fork "))
        return 0;

    int late = (int)call3(SYS_open, (s64)"/events/process", O_NONBLOCK, 0);
    if (late < 0 || call3(SYS_read, late, (s64)left, sizeof(left)) != -EAGAIN)
        return 0;

    struct { int fd; short events; short revents; } pollfd = { first, POLLIN, 0 };
    if (call3(SYS_poll, (s64)&pollfd, 1, 2000) <= 0 || !(pollfd.revents & POLLIN))
        return 0;
    left_size = call3(SYS_read, first, (s64)left, sizeof(left));
    s64 late_size = call3(SYS_read, late, (s64)right, sizeof(right));
    if (!contains(left, left_size, "exit ") || !contains(right, late_size, "exit "))
        return 0;

    (void)call4(SYS_wait4, child, 0, 0, 0);
    (void)call1(SYS_close, late);
    (void)call1(SYS_close, second);
    (void)call1(SYS_close, first);
    return 1;
}

static int file_stream_test(void) {
    static const char old_path[] = "/tmp/eventfs a\nb";
    static const char new_path[] = "/tmp/eventfs c";
    int events = (int)call3(SYS_open, (s64)"/events/files", O_NONBLOCK, 0);
    if (events < 0) return 0;
    int file = (int)call3(SYS_open, (s64)old_path,
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0 || call3(SYS_write, file, (s64)"x", 1) != 1) return 0;
    (void)call1(SYS_close, file);
    if (call2(SYS_rename, (s64)old_path, (s64)new_path) != 0) return 0;
    if (call1(SYS_unlink, (s64)new_path) != 0) return 0;

    char output[4096];
    s64 length = call3(SYS_read, events, (s64)output, sizeof(output));
    if (!contains(output, length, "create ") || !contains(output, length, "write ") ||
        !contains(output, length, "rename ") || !contains(output, length, "remove ") ||
        !contains(output, length, "/tmp/eventfs\\ a\\nb")) return 0;
    (void)call1(SYS_close, events);
    return 1;
}

static int overflow_test(void) {
    int events = (int)call3(SYS_open, (s64)"/events/files", O_NONBLOCK, 0);
    int file = (int)call3(SYS_open, (s64)"/tmp/eventfs-overflow",
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (events < 0 || file < 0) return 0;
    char output[4096];
    (void)call3(SYS_read, events, (s64)output, sizeof(output));
    for (unsigned index = 0; index < 1000U; index++)
        if (call3(SYS_write, file, (s64)"x", 1) != 1) return 0;
    int saw_loss = 0;
    for (unsigned attempt = 0; attempt < 8U; attempt++) {
        s64 length = call3(SYS_read, events, (s64)output, sizeof(output));
        if (length == -EAGAIN) break;
        if (contains(output, length, "lost ")) saw_loss = 1;
    }
    (void)call1(SYS_close, file);
    (void)call1(SYS_unlink, (s64)"/tmp/eventfs-overflow");
    (void)call1(SYS_close, events);
    return saw_loss;
}

static unsigned short network_port(unsigned short port) {
    return (unsigned short)((port << 8) | (port >> 8));
}

static int network_stream_test(void) {
    struct sockaddr_in {
        unsigned short family;
        unsigned short port;
        unsigned address;
        unsigned char zero[8];
    } address = { AF_INET, network_port(24567), 0x0100007fU, {0} };
    int events = (int)call3(SYS_open, (s64)"/events/network", O_NONBLOCK, 0);
    int listener = (int)call3(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (events < 0 || listener < 0 ||
        call3(SYS_bind, listener, (s64)&address, sizeof(address)) != 0 ||
        call2(SYS_listen, listener, 4) != 0) return 0;

    s64 child = call1(SYS_fork, 0);
    if (child == 0) {
        (void)call1(SYS_close, listener);
        int socket = (int)call3(SYS_socket, AF_INET, SOCK_STREAM, 0);
        if (socket >= 0)
            (void)call3(SYS_connect, socket, (s64)&address, sizeof(address));
        sleep_ms(50);
        if (socket >= 0) (void)call1(SYS_close, socket);
        (void)call1(SYS_exit_group, 0);
        for (;;) { }
    }
    if (child < 0) return 0;
    int accepted = (int)call3(SYS_accept, listener, 0, 0);
    if (accepted < 0) return 0;
    (void)call4(SYS_wait4, child, 0, 0, 0);
    (void)call1(SYS_close, accepted);

    char output[2048];
    s64 length = call3(SYS_read, events, (s64)output, sizeof(output));
    int passed = contains(output, length, "connect ") &&
                 contains(output, length, "accept ") &&
                 contains(output, length, "close ") &&
                 contains(output, length, "tcp 127.0.0.1:");
    (void)call1(SYS_close, listener);
    (void)call1(SYS_close, events);
    return passed;
}

static int close_race_test(void) {
    int file = (int)call3(SYS_open, (s64)"/tmp/eventfs-race",
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0) return 0;
    s64 children[4];
    for (unsigned index = 0; index < 4U; index++) {
        children[index] = call1(SYS_fork, 0);
        if (children[index] == 0) {
            for (unsigned write = 0; write < 300U; write++)
                (void)call3(SYS_write, file, (s64)"r", 1);
            (void)call1(SYS_exit_group, 0);
            for (;;) { }
        }
        if (children[index] < 0) return 0;
    }
    for (unsigned index = 0; index < 500U; index++) {
        int events = (int)call3(SYS_open, (s64)"/events/files", O_NONBLOCK, 0);
        if (events >= 0) (void)call1(SYS_close, events);
    }
    for (unsigned index = 0; index < 4U; index++)
        (void)call4(SYS_wait4, children[index], 0, 0, 0);
    (void)call1(SYS_close, file);
    (void)call1(SYS_unlink, (s64)"/tmp/eventfs-race");
    return 1;
}

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    int process_ok = process_stream_test();
    int files_ok = file_stream_test();
    int overflow_ok = overflow_test();
    int network_ok = network_stream_test();
    int race_ok = close_race_test();
    print(process_ok ? "EVENTFS process PASS\n" : "EVENTFS process FAIL\n");
    print(files_ok ? "EVENTFS files PASS\n" : "EVENTFS files FAIL\n");
    print(overflow_ok ? "EVENTFS overflow PASS\n" : "EVENTFS overflow FAIL\n");
    print(network_ok ? "EVENTFS network PASS\n" : "EVENTFS network FAIL\n");
    print(race_ok ? "EVENTFS race PASS\n" : "EVENTFS race FAIL\n");
    print(process_ok && files_ok && overflow_ok && network_ok && race_ok
          ? "EVENTFSTEST PASS\n" : "EVENTFSTEST FAIL\n");
    print("EVENTFSTEST DONE\n");
    for (;;) sleep_ms(1000);
}

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call run\n"
        "    hlt\n");
