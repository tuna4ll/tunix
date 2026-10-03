typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_IOCTL 16
#define SYS_EPOLL_WAIT 232
#define SYS_EPOLL_CTL 233
#define SYS_EPOLL_CREATE1 291
#define SYS_EXIT_GROUP 231

#define O_RDWR 2
#define O_NOCTTY 0400
#define O_NONBLOCK 04000
#define TCGETS 0x5401UL
#define TIOCGPTN 0x80045430UL
#define TIOCSPTLCK 0x40045431UL
#define EPOLL_CTL_ADD 1
#define EPOLLIN 1U
#define EAGAIN 11

#define THREAD_FLAGS 0x10F00UL
#define ROUNDS 200000U

#if defined(__x86_64__)
struct epoll_event {
    unsigned events;
    u64 data;
} __attribute__((packed));
#else
struct epoll_event {
    unsigned events;
    u64 data;
};
#endif

static char stack[65536] __attribute__((aligned(16)));
static volatile unsigned stop;
static volatile unsigned finished;
static int slave;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)text, (s64)length);
}

static void put_number(u64 value) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    char out[24];
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
    put(out);
}

static void contender(void) {
    char termios[64];
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE))
        (void)syscall3(SYS_IOCTL, slave, TCGETS, (s64)termios);
    __atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
}

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    int master = (int)syscall3(SYS_OPEN, (s64)"/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    int unlock = 0;
    unsigned number = 0;
    if (master < 0 || syscall3(SYS_IOCTL, master, TIOCSPTLCK, (s64)&unlock) != 0 ||
        syscall3(SYS_IOCTL, master, TIOCGPTN, (s64)&number) != 0 || number > 99) {
        put("PTYPOLL cannot open a pty\nPTYPOLL FAIL\n");
        syscall1(SYS_EXIT_GROUP, 1);
    }
    char path[] = "/dev/pts/00";
    if (number < 10) path[9] = (char)('0' + number), path[10] = 0;
    else path[9] = (char)('0' + number / 10U), path[10] = (char)('0' + number % 10U);
    slave = (int)syscall3(SYS_OPEN, (s64)path, O_RDWR | O_NOCTTY, 0);
    int poller = (int)syscall1(SYS_EPOLL_CREATE1, 0);
    struct epoll_event watch = {EPOLLIN, 7};
    if (slave < 0 || poller < 0 ||
        syscall4(SYS_EPOLL_CTL, poller, EPOLL_CTL_ADD, master, (s64)&watch) != 0) {
        put("PTYPOLL cannot watch the master\nPTYPOLL FAIL\n");
        syscall1(SYS_EXIT_GROUP, 1);
    }

    char *top = stack + sizeof(stack) - 8;
    *(void **)top = (void *)contender;
    if (spawn_thread(THREAD_FLAGS, top) < 0) {
        put("PTYPOLL cannot start the contender\nPTYPOLL FAIL\n");
        syscall1(SYS_EXIT_GROUP, 1);
    }

    u64 false_ready = 0;
    for (unsigned round = 0; round < ROUNDS; round++) {
        struct epoll_event event;
        if (syscall4(SYS_EPOLL_WAIT, poller, (s64)&event, 1, 0) != 1) continue;
        char byte;
        if (syscall3(SYS_READ, master, (s64)&byte, 1) == -EAGAIN) false_ready++;
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&finished, __ATOMIC_ACQUIRE)) { }

    const char *hello = "hi\n";
    (void)syscall3(SYS_WRITE, slave, (s64)hello, 3);
    struct epoll_event event = {0, 0};
    s64 seen = syscall4(SYS_EPOLL_WAIT, poller, (s64)&event, 1, 1000);
    char buffer[16];
    s64 got = syscall3(SYS_READ, master, (s64)buffer, sizeof(buffer));

    put("PTYPOLL false_ready=");
    put_number(false_ready);
    put(" wake=");
    put_number(seen == 1 && event.data == 7 && got > 0);
    put("\n");
    put(!false_ready && seen == 1 && got > 0 ? "PTYPOLL PASS\n" : "PTYPOLL FAIL\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
