typedef unsigned long u64;
typedef long s64;
typedef int s32;
typedef unsigned short u16;

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_ioctl 16
#define SYS_nanosleep 35
#define SYS_exit_group 231

#define O_RDONLY 0
#define EV_KEY 1
#define EV_ABS 3
#define ABS_X 0
#define ABS_Y 1
#define BTN_LEFT 0x110

#include "tunix_syscall.h"

#define IOC(dir, type, nr, size) \
    ((u64)(((dir) << 30) | ((size) << 16) | ((type) << 8) | (nr)))
#define EVIOCGBIT(type, size) IOC(2U, 'E', 0x20U + (type), (size))
#define EVIOCGABS(axis) IOC(2U, 'E', 0x40U + (axis), 24U)
#define EVIOCGNAME(size) IOC(2U, 'E', 0x06U, (size))

struct event { s64 tv_sec; s64 tv_usec; u16 type; u16 code; s32 value; };

static unsigned failures;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
}

static void put_number(s64 value) {
    char buffer[24];
    int index = (int)sizeof(buffer);
    u64 magnitude = value < 0 ? (u64)-value : (u64)value;
    buffer[--index] = 0;
    do {
        buffer[--index] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) buffer[--index] = '-';
    put(buffer + index);
}

static void check(const char *name, int ok, s64 detail) {
    put("TABLETTEST ");
    put(name);
    put(ok ? " PASS\n" : " FAIL ");
    if (!ok) {
        put_number(detail);
        put("\n");
        failures++;
    }
}

static int has_bit(const unsigned char *bits, unsigned bit) {
    return (bits[bit / 8U] >> (bit % 8U)) & 1U;
}

void run(void) {
    int fd = (int)syscall3(SYS_open, (s64)"/dev/input/event2", O_RDONLY, 0);
    check("open", fd >= 0, fd);
    char name[32];
    for (unsigned i = 0; i < sizeof(name); i++) name[i] = 0;
    (void)syscall3(SYS_ioctl, fd, (s64)EVIOCGNAME(sizeof(name)), (s64)name);
    check("named", name[0] == 'T' && name[6] == 'U' && name[10] == 'T', name[0]);
    unsigned char bits[96];
    for (unsigned i = 0; i < sizeof(bits); i++) bits[i] = 0;
    s64 got = syscall3(SYS_ioctl, fd, (s64)EVIOCGBIT(0, sizeof(bits)), (s64)bits);
    check("event-types", got > 0 && has_bit(bits, EV_ABS) && has_bit(bits, EV_KEY), got);
    for (unsigned i = 0; i < sizeof(bits); i++) bits[i] = 0;
    got = syscall3(SYS_ioctl, fd, (s64)EVIOCGBIT(EV_ABS, sizeof(bits)), (s64)bits);
    check("abs-axes", got > 0 && has_bit(bits, ABS_X) && has_bit(bits, ABS_Y), got);
    for (unsigned i = 0; i < sizeof(bits); i++) bits[i] = 0;
    got = syscall3(SYS_ioctl, fd, (s64)EVIOCGBIT(EV_KEY, sizeof(bits)), (s64)bits);
    check("left-button", got > 0 && has_bit(bits, BTN_LEFT), got);
    s32 absinfo[6] = {0, 0, 0, 0, 0, 0};
    got = syscall3(SYS_ioctl, fd, (s64)EVIOCGABS(ABS_X), (s64)absinfo);
    check("abs-range", got == 0 && absinfo[1] == 0 && absinfo[2] == 32767, absinfo[2]);

    put("TABLETTEST READY\n");
    s32 x = -1, y = -1, pressed = -1, released = -1;
    struct event events[16];
    while (released < 0) {
        s64 length = syscall3(SYS_read, fd, (s64)events, sizeof(events));
        if (length <= 0) {
            check("read", 0, length);
            break;
        }
        for (s64 i = 0; i < length / (s64)sizeof(struct event); i++) {
            if (events[i].type == EV_ABS && events[i].code == ABS_X) x = events[i].value;
            if (events[i].type == EV_ABS && events[i].code == ABS_Y) y = events[i].value;
            if (events[i].type == EV_KEY && events[i].code == BTN_LEFT) {
                if (events[i].value == 1) pressed = 1;
                else released = 1;
            }
        }
    }
    check("absolute-x", x == 8000, x);
    check("absolute-y", y == 24000, y);
    check("click", pressed == 1 && released == 1, pressed);
    put(failures ? "TABLETTEST FAIL\n" : "TABLETTEST PASS\n");
    syscall1(SYS_exit_group, 0);
    for (;;) { }
}

TUNIX_START(run)
