typedef unsigned long u64;
typedef long s64;

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_pread64 17
#define SYS_pwrite64 18
#define SYS_nanosleep 35
#define SYS_fork 57
#define SYS_wait4 61
#define SYS_exit_group 231
#define SYS_clock_gettime 228

#define CLOCK_MONOTONIC 1
#define O_RDONLY 0
#define O_RDWR 2
#define O_NONBLOCK 04000

#include "tunix_syscall.h"

#define WINDOW_NS 75000000000UL
#define BLOCK 4096
#define BLOCKS 256
#define EV_KEY 1
#define EV_REL 2
#define KEY_MAX 256

struct event {
    s64 tv_sec;
    s64 tv_usec;
    unsigned short type;
    unsigned short code;
    int value;
};

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
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

static unsigned char written[BLOCK];
static unsigned char readback[BLOCK];

static void disk_loop(const char *path, u64 deadline, int wait_for_it) {
    int fd = (int)syscall3(SYS_open, (s64)path, O_RDWR, 0);
    while (fd < 0 && wait_for_it && now_ns() < deadline) {
        sleep_ns(250000000UL);
        fd = (int)syscall3(SYS_open, (s64)path, O_RDWR, 0);
    }
    if (fd < 0) {
        put("USBDISK ");
        put(path);
        put(" open failed\n");
        syscall1(SYS_exit_group, 0);
    }
    u64 ok = 0, bad = 0, errors = 0, round = 0, failing = 0;
    while (now_ns() < deadline && failing < 32) {
        for (u64 block = 0; block < BLOCKS && now_ns() < deadline; block++) {
            for (int at = 0; at < BLOCK; at++)
                written[at] = (unsigned char)(round * 131 + block * 7 + (u64)at);
            s64 offset = (s64)(block * BLOCK);
            if (syscall4(SYS_pwrite64, fd, (s64)written, BLOCK, offset) != BLOCK ||
                syscall4(SYS_pread64, fd, (s64)readback, BLOCK, offset) != BLOCK) {
                errors++;
                if (++failing >= 32) break;
                continue;
            }
            failing = 0;
            int same = 1;
            for (int at = 0; at < BLOCK; at++)
                if (readback[at] != written[at]) same = 0;
            if (same) ok++;
            else bad++;
        }
        round++;
    }
    static char line[160];
    unsigned at = 0;
    const char *parts[3] = { "USBDISK ", path, " ok=" };
    for (int part = 0; part < 3; part++)
        for (const char *c = parts[part]; *c; c++) line[at++] = *c;
    u64 values[3] = { ok, bad, errors };
    const char *labels[3] = { "", " bad=", " errors=" };
    for (int index = 0; index < 3; index++) {
        for (const char *c = labels[index]; *c; c++) line[at++] = *c;
        char digits[24];
        int count = 0;
        u64 value = values[index];
        do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
        while (count) line[at++] = digits[--count];
    }
    line[at++] = '\n';
    line[at] = 0;
    put(line);
    syscall1(SYS_exit_group, 0);
}

static unsigned presses[KEY_MAX];
static unsigned releases[KEY_MAX];

static void count(struct event *batch, s64 got, u64 *rel) {
    for (s64 at = 0; at + (s64)sizeof(batch[0]) <= got; at += (s64)sizeof(batch[0])) {
        struct event *one = batch + at / (s64)sizeof(batch[0]);
        if (one->type == EV_REL) (*rel)++;
        if (one->type != EV_KEY || one->code >= KEY_MAX) continue;
        if (one->code >= 0x110 && one->code < 0x118) continue;
        if (one->value == 1) presses[one->code]++;
        else if (one->value == 0) releases[one->code]++;
    }
}

static void run_all(void) {
    int keyboard = (int)syscall3(SYS_open, (s64)"/dev/input/event0", O_RDWR | O_NONBLOCK, 0);
    int mouse = -1;
    u64 deadline = now_ns() + WINDOW_NS;
    static const char *const paths[3] = { "/dev/sdb", "/dev/sdc", "/dev/sdd" };
    s64 children[3];
    for (int index = 0; index < 3; index++) {
        children[index] = syscall1(SYS_fork, 0);
        if (children[index] == 0) disk_loop(paths[index], deadline - 3000000000UL, index == 2);
    }
    put("USBTEST READY\n");

    struct event batch[32];
    u64 rel = 0;
    static unsigned mouse_buttons[2];
    while (now_ns() < deadline) {
        if (mouse < 0)
            mouse = (int)syscall3(SYS_open, (s64)"/dev/input/event1", O_RDWR | O_NONBLOCK, 0);
        int idle = 1;
        if (keyboard >= 0) {
            s64 got = syscall3(SYS_read, keyboard, (s64)batch, sizeof(batch));
            if (got > 0) {
                idle = 0;
                count(batch, got, &rel);
            }
        }
        if (mouse >= 0) {
            s64 got = syscall3(SYS_read, mouse, (s64)batch, sizeof(batch));
            if (got > 0) {
                idle = 0;
                for (s64 at = 0; at < got / (s64)sizeof(batch[0]); at++) {
                    if (batch[at].type == EV_REL) rel++;
                    if (batch[at].type == EV_KEY && batch[at].value == 1) mouse_buttons[0]++;
                    if (batch[at].type == EV_KEY && batch[at].value == 0) mouse_buttons[1]++;
                }
            }
        }
        if (idle) sleep_ns(2000000UL);
    }
    for (int index = 0; index < 3; index++) (void)syscall4(SYS_wait4, children[index], 0, 0, 0);

    for (unsigned code = 0; code < KEY_MAX; code++) {
        if (!presses[code] && !releases[code]) continue;
        put("USBKEY code=");
        put_number(code);
        put(" press=");
        put_number(presses[code]);
        put(" release=");
        put_number(releases[code]);
        put("\n");
    }
    put("USBMOUSE rel=");
    put_number(rel);
    put(" press=");
    put_number(mouse_buttons[0]);
    put(" release=");
    put_number(mouse_buttons[1]);
    put("\n");

    int interrupts = (int)syscall3(SYS_open, (s64)"/proc/interrupts", O_RDONLY, 0);
    if (interrupts >= 0) {
        static char text[4096];
        s64 got = syscall3(SYS_read, interrupts, (s64)text, sizeof(text) - 1);
        if (got > 0) {
            text[got] = 0;
            char *line = text;
            for (s64 at = 0; at <= got; at++) {
                if (text[at] != '\n' && text[at] != 0) continue;
                text[at] = 0;
                for (char *scan = line; *scan; scan++) {
                    if (scan[0] == 'x' && scan[1] == 'h' && scan[2] == 'c' && scan[3] == 'i') {
                        put("USBIRQ ");
                        put(line);
                        put("\n");
                        break;
                    }
                }
                line = text + at + 1;
            }
        }
    }
    put("USBTEST DONE\n");
}

static void run_and_park(void) __attribute__((noreturn, used));
static void run_and_park(void) {
    run_all();
    for (;;) sleep_ns(1000000000UL);
}

TUNIX_START(run_and_park)
