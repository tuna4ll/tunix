#ifndef TUNIX_TEST_H
#define TUNIX_TEST_H

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#if defined(__x86_64__)
#define SYS_read            0
#define SYS_write           1
#define SYS_close           3
#define SYS_fstat           5
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_rt_sigaction    13
#define SYS_pread64         17
#define SYS_nanosleep       35
#define SYS_sync            162
#define SYS_getpid          39
#define SYS_sendmsg         46
#define SYS_recvmsg         47
#define SYS_socketpair      53
#define SYS_clone           56
#define SYS_execve          59
#define SYS_wait4           61
#define SYS_kill            62
#define SYS_ftruncate       77
#define SYS_getppid         110
#define SYS_reboot          169
#define SYS_getdents64      217
#define SYS_clock_gettime   228
#define SYS_exit_group      231
#define SYS_epoll_ctl       233
#define SYS_openat          257
#define SYS_mkdirat         258
#define SYS_unlinkat        263
#define SYS_renameat        264
#define SYS_epoll_pwait     281
#define SYS_timerfd_create  283
#define SYS_timerfd_settime 286
#define SYS_eventfd2        290
#define SYS_epoll_create1   291
#define SYS_dup3            292
#define SYS_pipe2           293
#define SA_RESTORER         0x04000000UL
#elif defined(__aarch64__)
#define SYS_eventfd2        19
#define SYS_epoll_create1   20
#define SYS_epoll_ctl       21
#define SYS_epoll_pwait     22
#define SYS_dup3            24
#define SYS_mkdirat         34
#define SYS_unlinkat        35
#define SYS_renameat        38
#define SYS_ftruncate       46
#define SYS_openat          56
#define SYS_close           57
#define SYS_pipe2           59
#define SYS_getdents64      61
#define SYS_lseek           62
#define SYS_read            63
#define SYS_write           64
#define SYS_pread64         67
#define SYS_fstat           80
#define SYS_timerfd_create  85
#define SYS_timerfd_settime 86
#define SYS_exit_group      94
#define SYS_nanosleep       101
#define SYS_sync            81
#define SYS_clock_gettime   113
#define SYS_kill            129
#define SYS_rt_sigaction    134
#define SYS_reboot          142
#define SYS_getpid          172
#define SYS_getppid         173
#define SYS_socketpair      199
#define SYS_sendmsg         211
#define SYS_recvmsg         212
#define SYS_brk             214
#define SYS_munmap          215
#define SYS_clone           220
#define SYS_execve          221
#define SYS_mmap            222
#define SYS_mprotect        226
#define SYS_wait4           260
#define SA_RESTORER         0UL
#else
#error "tests are written for x86_64 and aarch64"
#endif

#define AT_FDCWD     (-100)
#define AT_REMOVEDIR 0x200

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_EXCL     0200
#define O_TRUNC    01000
#define O_APPEND   02000
#define O_NONBLOCK 04000
#define O_CLOEXEC  02000000

#define ENOENT    2
#define EBADF     9
#define ECHILD    10
#define EAGAIN    11
#define EEXIST    17
#define EPIPE     32
#define ENOTEMPTY 39

#define PROT_READ     1
#define PROT_WRITE    2
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGCHLD 17
#define SIG_IGN 1UL
#define WNOHANG 1

#define AF_UNIX     1
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOL_SOCKET  1
#define SCM_RIGHTS  1

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define EFD_SEMAPHORE   1
#define EFD_NONBLOCK    O_NONBLOCK
#define EPOLL_CTL_ADD   1
#define EPOLLIN         0x001U

#define PAGE_SIZE 4096UL

struct timespec {
    s64 sec;
    s64 nsec;
};

struct itimerspec {
    struct timespec interval;
    struct timespec value;
};

struct sigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

struct iovec {
    void *base;
    u64 length;
};

struct msghdr {
    void *name;
    u32 name_length;
    struct iovec *iov;
    u64 iov_count;
    void *control;
    u64 control_length;
    int flags;
};

struct cmsghdr {
    u64 length;
    int level;
    int type;
};

struct dirent64 {
    u64 inode;
    s64 offset;
    u16 record_length;
    u8 type;
    char name[];
};

#if defined(__x86_64__)
struct __attribute__((packed)) epoll_event {
    u32 events;
    u64 data;
};
#define STAT_MODE_OFFSET 24
#else
struct epoll_event {
    u32 events;
    u64 data;
};
#define STAT_MODE_OFFSET 16
#endif
#define STAT_SIZE_OFFSET 48

static inline s64 syscall6(s64 number, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
#if defined(__x86_64__)
    register s64 r10 __asm__("r10") = d;
    register s64 r8 __asm__("r8") = e;
    register s64 r9 __asm__("r9") = f;
    s64 result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return result;
#else
    register s64 x8 __asm__("x8") = number;
    register s64 x0 __asm__("x0") = a;
    register s64 x1 __asm__("x1") = b;
    register s64 x2 __asm__("x2") = c;
    register s64 x3 __asm__("x3") = d;
    register s64 x4 __asm__("x4") = e;
    register s64 x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
#endif
}

#define SYSCALL(n, a, b, c, d, e, f) \
    syscall6((n), (s64)(a), (s64)(b), (s64)(c), (s64)(d), (s64)(e), (s64)(f))

static inline s64 read(int fd, void *buffer, u64 length) {
    return SYSCALL(SYS_read, fd, buffer, length, 0, 0, 0);
}

static inline s64 write(int fd, const void *buffer, u64 length) {
    return SYSCALL(SYS_write, fd, buffer, length, 0, 0, 0);
}

static inline s64 pread(int fd, void *buffer, u64 length, s64 offset) {
    return SYSCALL(SYS_pread64, fd, buffer, length, offset, 0, 0);
}

static inline s64 open(const char *path, int flags, int mode) {
    return SYSCALL(SYS_openat, AT_FDCWD, path, flags, mode, 0, 0);
}

static inline s64 close(int fd) { return SYSCALL(SYS_close, fd, 0, 0, 0, 0, 0); }

static inline s64 lseek(int fd, s64 offset, int whence) {
    return SYSCALL(SYS_lseek, fd, offset, whence, 0, 0, 0);
}

static inline s64 ftruncate(int fd, s64 length) {
    return SYSCALL(SYS_ftruncate, fd, length, 0, 0, 0, 0);
}

static inline s64 fstat_size(int fd) {
    u64 buffer[32];
    s64 result = SYSCALL(SYS_fstat, fd, buffer, 0, 0, 0, 0);
    if (result < 0) return result;
    return *(s64 *)((u8 *)buffer + STAT_SIZE_OFFSET);
}

static inline s64 mkdir(const char *path, int mode) {
    return SYSCALL(SYS_mkdirat, AT_FDCWD, path, mode, 0, 0, 0);
}

static inline s64 unlink(const char *path) {
    return SYSCALL(SYS_unlinkat, AT_FDCWD, path, 0, 0, 0, 0);
}

static inline s64 rmdir(const char *path) {
    return SYSCALL(SYS_unlinkat, AT_FDCWD, path, AT_REMOVEDIR, 0, 0, 0);
}

static inline s64 rename(const char *from, const char *to) {
    return SYSCALL(SYS_renameat, AT_FDCWD, from, AT_FDCWD, to, 0, 0);
}

static inline s64 getdents64(int fd, void *buffer, u64 length) {
    return SYSCALL(SYS_getdents64, fd, buffer, length, 0, 0, 0);
}

static inline void *mmap(void *address, u64 length, int protection, int flags) {
    return (void *)SYSCALL(SYS_mmap, address, length, protection, flags, -1, 0);
}

static inline s64 munmap(void *address, u64 length) {
    return SYSCALL(SYS_munmap, address, length, 0, 0, 0, 0);
}

static inline s64 mprotect(void *address, u64 length, int protection) {
    return SYSCALL(SYS_mprotect, address, length, protection, 0, 0, 0);
}

static inline u64 brk(u64 end) { return (u64)SYSCALL(SYS_brk, end, 0, 0, 0, 0, 0); }

static inline s64 getpid(void) { return SYSCALL(SYS_getpid, 0, 0, 0, 0, 0, 0); }

static inline s64 getppid(void) { return SYSCALL(SYS_getppid, 0, 0, 0, 0, 0, 0); }

static inline s64 fork(void) { return SYSCALL(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0); }

static inline s64 execve(const char *path, char *const argv[], char *const envp[]) {
    return SYSCALL(SYS_execve, path, argv, envp, 0, 0, 0);
}

static inline s64 waitpid(s64 pid, int *status, int options) {
    return SYSCALL(SYS_wait4, pid, status, options, 0, 0, 0);
}

static inline s64 kill(s64 pid, int signal) { return SYSCALL(SYS_kill, pid, signal, 0, 0, 0, 0); }

__attribute__((noreturn)) static inline void exit(int code) {
    for (;;) SYSCALL(SYS_exit_group, code, 0, 0, 0, 0, 0);
}

static inline s64 pipe2(int fds[2], int flags) {
    return SYSCALL(SYS_pipe2, fds, flags, 0, 0, 0, 0);
}

static inline s64 dup3(int from, int to, int flags) {
    return SYSCALL(SYS_dup3, from, to, flags, 0, 0, 0);
}

static inline s64 socketpair(int domain, int type, int fds[2]) {
    return SYSCALL(SYS_socketpair, domain, type, 0, fds, 0, 0);
}

static inline s64 sendmsg(int fd, const struct msghdr *message, int flags) {
    return SYSCALL(SYS_sendmsg, fd, message, flags, 0, 0, 0);
}

static inline s64 recvmsg(int fd, struct msghdr *message, int flags) {
    return SYSCALL(SYS_recvmsg, fd, message, flags, 0, 0, 0);
}

static inline s64 sync(void) { return SYSCALL(SYS_sync, 0, 0, 0, 0, 0, 0); }

static inline s64 nanosleep(const struct timespec *duration) {
    return SYSCALL(SYS_nanosleep, duration, 0, 0, 0, 0, 0);
}

static inline s64 clock_gettime(int clock, struct timespec *now) {
    return SYSCALL(SYS_clock_gettime, clock, now, 0, 0, 0, 0);
}

static inline s64 eventfd(u32 initial, int flags) {
    return SYSCALL(SYS_eventfd2, initial, flags, 0, 0, 0, 0);
}

static inline s64 epoll_create(void) { return SYSCALL(SYS_epoll_create1, 0, 0, 0, 0, 0, 0); }

static inline s64 epoll_add(int epoll, int fd, u32 events, u64 data) {
    struct epoll_event event = {events, data};
    return SYSCALL(SYS_epoll_ctl, epoll, EPOLL_CTL_ADD, fd, &event, 0, 0);
}

static inline s64 epoll_wait(int epoll, struct epoll_event *events, int count, int timeout) {
    return SYSCALL(SYS_epoll_pwait, epoll, events, count, timeout, 0, 8);
}

static inline s64 timerfd_create(int clock) {
    return SYSCALL(SYS_timerfd_create, clock, 0, 0, 0, 0, 0);
}

static inline s64 timerfd_settime(int fd, const struct itimerspec *value) {
    return SYSCALL(SYS_timerfd_settime, fd, 0, value, 0, 0, 0);
}

extern void test_signal_return(void);

static inline s64 signal(int number, u64 handler) {
    struct sigaction action = {handler, SA_RESTORER, (u64)test_signal_return, 0};
    return SYSCALL(SYS_rt_sigaction, number, &action, 0, 8, 0, 0);
}

void *memset(void *destination, int value, u64 length) {
    u8 *bytes = destination;
    while (length--) *bytes++ = (u8)value;
    return destination;
}

void *memcpy(void *destination, const void *source, u64 length) {
    u8 *to = destination;
    const u8 *from = source;
    while (length--) *to++ = *from++;
    return destination;
}

static inline u64 strlen(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    return length;
}

static inline int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static inline int memeq(const void *a, const void *b, u64 length) {
    const u8 *left = a, *right = b;
    for (u64 i = 0; i < length; i++)
        if (left[i] != right[i]) return 0;
    return 1;
}

static inline s64 elapsed_ms(const struct timespec *start, const struct timespec *end) {
    return (end->sec - start->sec) * 1000 + (end->nsec - start->nsec) / 1000000;
}

static inline s64 now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.sec * 1000 + now.nsec / 1000000;
}

static inline void sleep_ms(s64 milliseconds) {
    struct timespec duration = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    nanosleep(&duration);
}

static inline int exited_with(int status) { return (status & 0x7f) ? -1 : (status >> 8) & 0xff; }

static inline int killed_by(int status) { return status & 0x7f; }

static unsigned checks_run;
static unsigned checks_failed;

static inline void print(const char *text) { write(1, text, strlen(text)); }

static inline void print_number(s64 value) {
    char digits[24];
    int count = 0;
    u64 magnitude = value < 0 ? (u64)-value : (u64)value;
    do {
        digits[sizeof(digits) - 1 - count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) digits[sizeof(digits) - 1 - count++] = '-';
    write(1, digits + sizeof(digits) - count, (u64)count);
}

static inline void report(int passed, const char *what) {
    checks_run++;
    if (!passed) checks_failed++;
    print(TEST_NAME);
    print(passed ? ": ok    " : ": FAIL  ");
    print(what);
}

static inline void expect(int passed, const char *what) {
    report(passed, what);
    print("\n");
}

static inline void expect_eq(s64 actual, s64 wanted, const char *what) {
    report(actual == wanted, what);
    if (actual != wanted) {
        print(" (got ");
        print_number(actual);
        print(", wanted ");
        print_number(wanted);
        print(")");
    }
    print("\n");
}

static void run(int argc, char **argv);

__attribute__((used, noreturn)) static void test_start(u64 *stack) {
    int argc = (int)stack[0];
    char **argv = (char **)(stack + 1);
    run(argc, argv);
    print(TEST_NAME);
    print(checks_failed ? ": FAIL " : ": PASS ");
    print_number(checks_run - checks_failed);
    print("/");
    print_number(checks_run);
    print("\n");
    if (getpid() == 1) SYSCALL(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
    exit(checks_failed ? 1 : 0);
}

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call test_start\n"
        "    hlt\n"
        ".globl test_signal_return\n"
        "test_signal_return:\n"
        "    mov $15, %eax\n"
        "    syscall\n"
        "    hlt\n");
#else
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov x29, #0\n"
        "    mov x0, sp\n"
        "    bl test_start\n"
        "    b .\n"
        ".globl test_signal_return\n"
        "test_signal_return:\n"
        "    mov x8, #139\n"
        "    svc #0\n"
        "    b .\n");
#endif

#endif
