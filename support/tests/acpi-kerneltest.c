typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_WRITE 1
#define SYS_NANOSLEEP 35

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)text, (s64)length);
}

void run(void) {
    put("ACPITEST READY\n");
    struct { s64 seconds, nanoseconds; } request = { 1, 0 };
    for (;;) (void)syscall2(SYS_NANOSLEEP, (s64)&request, 0);
}

TUNIX_START(run)
