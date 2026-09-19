typedef unsigned long u64;
typedef long s64;

#define SYS_write 1
#define SYS_close 3
#define SYS_openat 257
#define SYS_setresuid 117
#define SYS_finit_module 313
#define SYS_exit_group 231

#define AT_FDCWD -100

#include "tunix_syscall.h"

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
}

static void put_signed(s64 value) {
    char digits[24];
    int count = 0;
    int negative = value < 0;
    u64 magnitude = negative ? (u64)(-value) : (u64)value;
    if (!magnitude) digits[count++] = '0';
    while (magnitude && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + magnitude % 10UL);
        magnitude /= 10UL;
    }
    char out[26];
    int at = 0;
    if (negative) out[at++] = '-';
    while (count-- > 0) out[at++] = digits[count];
    out[at] = '\0';
    put(out);
}

void start(void) {
    const char *path = "/usr/lib/modules/0.1.0/kernel/tunix_probe.ko";
    s64 dropped = syscall3(SYS_setresuid, 1000, 1000, 1000);
    s64 fd = syscall4(SYS_openat, AT_FDCWD, (s64)path, 0, 0);
    s64 status = fd < 0 ? fd : syscall3(SYS_finit_module, (s64)fd, (s64)"", 0);
    if (fd >= 0) (void)syscall1(SYS_close, (s64)fd);

    put("MODULEPERM dropped=");
    put_signed(dropped);
    put(" open=");
    put_signed(fd < 0 ? fd : 0);
    put(" finit=");
    put_signed(status);
    put("\n");
    (void)syscall1(SYS_exit_group, 0);
}

TUNIX_START(start)
