typedef unsigned long u64;
typedef long s64;

#define SYS_write 1
#define SYS_execve 59
#define SYS_exit_group 231

#include "tunix_syscall.h"

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
}

static char *const arguments[] = { (char *)"bash", (char *)"/moduletest.sh", 0 };
static char *const environment[] = {
    (char *)"PATH=/usr/bin:/usr/sbin",
    (char *)"HOME=/",
    (char *)"TERM=linux",
    0
};

void start(void) {
    put("MODULETEST INIT\n");
    (void)syscall3(SYS_execve, (s64)"/usr/bin/bash", (s64)arguments, (s64)environment);
    put("MODULETEST FAIL no shell\n");
    (void)syscall1(SYS_exit_group, 1);
}

TUNIX_START(start)
