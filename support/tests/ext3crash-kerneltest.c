typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_FSYNC 74
#define SYS_RENAME 82
#define SYS_MKDIR 83
#define SYS_UNLINK 87
#define SYS_MOUNT 165
#define SYS_EXIT_GROUP 231

#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000

#define FILE_BYTES 6000U

static char content[FILE_BYTES];

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

static void name_for(char *out, const char *prefix, u64 number) {
    unsigned at = 0;
    while (prefix[at]) {
        out[at] = prefix[at];
        at++;
    }
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + number % 10U);
        number /= 10U;
    } while (number);
    while (count) out[at++] = digits[--count];
    out[at] = 0;
}

static void fill(u64 number) {
    for (unsigned index = 0; index < FILE_BYTES; index++)
        content[index] = (char)('a' + (number * 7U + index) % 26U);
}

static int write_file(const char *path, u64 number, int durable) {
    int fd = (int)syscall3(SYS_OPEN, (s64)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    fill(number);
    int status = syscall3(SYS_WRITE, fd, (s64)content, FILE_BYTES) == FILE_BYTES ? 0 : -1;
    if (status == 0 && durable && syscall1(SYS_FSYNC, fd) != 0) status = -1;
    (void)syscall1(SYS_CLOSE, fd);
    return status;
}

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    (void)syscall2(SYS_MKDIR, (s64)"/crash", 0755);
    if (syscall6(SYS_MOUNT, (s64)"/dev/sdb", (s64)"/crash", (s64)"ext3", 0, 0, 0) != 0) {
        put("CRASH cannot mount /dev/sdb\nCRASH FAIL\n");
        syscall1(SYS_EXIT_GROUP, 1);
    }
    (void)syscall2(SYS_MKDIR, (s64)"/crash/kept", 0755);
    (void)syscall2(SYS_MKDIR, (s64)"/crash/churn", 0755);
    put("CRASH go\n");
    char path[64];
    char other[64];
    for (u64 number = 0;; number++) {
        name_for(path, "/crash/kept/f", number);
        if (write_file(path, number, 1) != 0) {
            put("CRASH write failed\nCRASH FAIL\n");
            syscall1(SYS_EXIT_GROUP, 1);
        }
        put("CRASH synced ");
        put_number(number);
        put("\n");
        for (u64 churn = 0; churn < 4; churn++) {
            name_for(path, "/crash/churn/t", number * 4U + churn);
            name_for(other, "/crash/churn/r", number * 4U + churn);
            (void)write_file(path, number + churn, 0);
            (void)syscall2(SYS_RENAME, (s64)path, (s64)other);
            if (churn & 1U) (void)syscall1(SYS_UNLINK, (s64)other);
        }
    }
}

TUNIX_START(run)
