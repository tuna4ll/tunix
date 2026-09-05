/* Whether a keystroke arrives once. */
/* Several processors read the same evdev device at once while keys are typed
   into the machine, which is the shape that used to lose an event ring's
   count and hand the same key out twice. */

typedef unsigned long u64;
typedef long s64;

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_nanosleep 35
#define SYS_fork 57
#define SYS_exit_group 231
#define SYS_clock_gettime 228
#define SYS_sched_setaffinity 203

#define CLOCK_MONOTONIC 1
#define O_RDWR 2
#define O_NONBLOCK 04000

static inline s64 syscall1(s64 n, s64 a) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}
static inline s64 syscall2(s64 n, s64 a, s64 b) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static inline s64 syscall3(s64 n, s64 a, s64 b, s64 c) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* Printed and also kept, so a machine with no serial cable can be read after. */
static int results_fd = -1;
#define O_WRONLY_CREAT_TRUNC 0x241

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
    if (results_fd >= 0) (void)syscall3(SYS_write, results_fd, (s64)text, (s64)length);
}

static void put_number(u64 value) {
    char buffer[24];
    int index = (int)sizeof(buffer);
    buffer[--index] = 0;
    do {
        buffer[--index] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    put(buffer + index);
}

static u64 now_ns(void) {
    struct { s64 seconds, nanoseconds; } value = {0, 0};
    (void)syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.seconds * 1000000000UL + (u64)value.nanoseconds;
}

static void sleep_ns(u64 nanoseconds) {
    struct { s64 seconds, nanoseconds; } request;
    request.seconds = (s64)(nanoseconds / 1000000000UL);
    request.nanoseconds = (s64)(nanoseconds % 1000000000UL);
    (void)syscall2(SYS_nanosleep, (s64)&request, 0);
}

static void pin_to_cpu(unsigned cpu) {
    u64 mask = 1UL << cpu;
    (void)syscall3(SYS_sched_setaffinity, 0, sizeof(mask), (s64)&mask);
}

struct event {
    s64 tv_sec;
    s64 tv_usec;
    unsigned short type;
    unsigned short code;
    int value;
};

#define EV_KEY 1
#define KEY_MAX 256
#define STRESS_READERS 3
#define WINDOW_NS 25000000000UL

static int open_keyboard(void) {
    return (int)syscall3(SYS_open, (s64)"/dev/input/event0", O_RDWR | O_NONBLOCK, 0);
}

/* One reader per processor, each with a device of its own, all reading at once:
   the readers are what put several processors inside the same driver. */
static void stress_reader(unsigned cpu) {
    pin_to_cpu(cpu);
    int fd = open_keyboard();
    struct event batch[16];
    for (;;) {
        if (fd >= 0) (void)syscall3(SYS_read, fd, (s64)batch, sizeof(batch));
    }
}

static unsigned presses[KEY_MAX];
static unsigned releases[KEY_MAX];
static unsigned repeats[KEY_MAX];

static int run_all(void) {
    results_fd = (int)syscall3(SYS_open, (s64)"/tunix-inputtest-results.txt",
                               O_WRONLY_CREAT_TRUNC, 0644);
    pin_to_cpu(0);

    int fd = open_keyboard();
    if (fd < 0) {
        put("INPUTTEST open failed\n");
        put("INPUTTEST DONE\n");
        return 1;
    }

    for (unsigned index = 0; index < STRESS_READERS; index++) {
        s64 child = syscall1(SYS_fork, 0);
        if (child == 0) stress_reader(index + 1U);
    }

    put("INPUTTEST READY\n");

    struct event batch[32];
    u64 deadline = now_ns() + WINDOW_NS;
    u64 total = 0;
    while (now_ns() < deadline) {
        s64 got = syscall3(SYS_read, fd, (s64)batch, sizeof(batch));
        if (got <= 0) {
            sleep_ns(1000000UL);
            continue;
        }
        for (s64 at = 0; at + (s64)sizeof(batch[0]) <= got; at += (s64)sizeof(batch[0])) {
            struct event *one = (struct event *)((char *)batch + at);
            if (one->type != EV_KEY || one->code >= KEY_MAX) continue;
            total++;
            if (one->value == 0) releases[one->code]++;
            else if (one->value == 1) presses[one->code]++;
            else repeats[one->code]++;
        }
    }

    for (unsigned code = 0; code < KEY_MAX; code++) {
        if (!presses[code] && !releases[code] && !repeats[code]) continue;
        put("INPUT code=");
        put_number(code);
        put(" press=");
        put_number(presses[code]);
        put(" release=");
        put_number(releases[code]);
        put(" repeat=");
        put_number(repeats[code]);
        put("\n");
    }
    put("INPUT total=");
    put_number(total);
    put("\n");
    put("INPUTTEST DONE\n");
    return 0;
}

/* Init returning is a panic, which is not the report anybody wants. */
static void run_and_park(void) __attribute__((noreturn, used));
static void run_and_park(void) {
    (void)run_all();
    for (;;) sleep_ns(1000000000UL);
}

/* The entry point aligns the stack itself, because there is no libc here. */
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call run_and_park\n"
        "    hlt\n");
