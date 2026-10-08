typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_FSYNC 74
#define SYS_CLOSE 3
#define SYS_MKDIR 83
#define SYS_MOUNT 165
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231

#define CLOCK_MONOTONIC 1
#define CHUNK_BYTES (1024 * 1024)
#define WRITTEN_BYTES (16U * 1024U * 1024U)
#define O_WRONLY_CREAT_TRUNC 01101

static unsigned char chunk[CHUNK_BYTES];

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

static u64 now_ns(void) {
    struct { s64 seconds, nanoseconds; } value;
    (void)syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.seconds * 1000000000ULL + (u64)value.nanoseconds;
}

static u64 parse_hex(const char *text, s64 length) {
    u64 value = 0;
    for (s64 index = 0; index < length; index++) {
        char c = text[index];
        if (c >= '0' && c <= '9') value = value * 16U + (u64)(c - '0');
        else if (c >= 'a' && c <= 'f') value = value * 16U + (u64)(c - 'a' + 10);
        else break;
    }
    return value;
}

static void finish(int ok) {
    put(ok ? "USBIO PASS\n" : "USBIO FAIL\n");
    syscall1(SYS_EXIT_GROUP, ok ? 0 : 1);
    for (;;) { }
}

void run(void) {
    (void)syscall2(SYS_MKDIR, (s64)"/stick", 0755);
    if (syscall6(SYS_MOUNT, (s64)"/dev/sdb", (s64)"/stick", (s64)"ext3", 0, 0, 0) != 0) {
        put("USBIO cannot mount /dev/sdb\n");
        finish(0);
    }
    char expected_text[32];
    int sum = (int)syscall3(SYS_OPEN, (s64)"/stick/sum", 0, 0);
    s64 sum_length = sum >= 0 ? syscall3(SYS_READ, sum, (s64)expected_text, sizeof(expected_text)) : -1;
    if (sum_length <= 0) {
        put("USBIO no checksum file\n");
        finish(0);
    }
    u64 expected = parse_hex(expected_text, sum_length);

    int fd = (int)syscall3(SYS_OPEN, (s64)"/stick/data", 0, 0);
    if (fd < 0) {
        put("USBIO no data file\n");
        finish(0);
    }
    u64 hash = 1469598103934665603ULL;
    u64 total = 0;
    u64 started = now_ns();
    for (;;) {
        s64 got = syscall3(SYS_READ, fd, (s64)chunk, CHUNK_BYTES);
        if (got < 0) {
            put("USBIO read error\n");
            finish(0);
        }
        if (got == 0) break;
        for (s64 index = 0; index < got; index++) {
            hash ^= chunk[index];
            hash *= 1099511628211ULL;
        }
        total += (u64)got;
    }
    u64 elapsed = now_ns() - started;
    u64 ms = elapsed / 1000000ULL;
    put("USBIO bytes ");
    put_number(total);
    put(" in ");
    put_number(ms);
    put(" ms, ");
    put_number(ms ? total * 1000ULL / ms / 1024ULL : 0);
    put(" KiB/s\n");
    put(hash == expected ? "USBIO checksum matches\n" : "USBIO checksum DIFFERS\n");
    int read_ok = hash == expected && total == 24U * 1024U * 1024U;

    int out = (int)syscall3(SYS_OPEN, (s64)"/stick/written", O_WRONLY_CREAT_TRUNC, 0644);
    if (out < 0) {
        put("USBIO cannot create the written file\n");
        finish(0);
    }
    started = now_ns();
    for (u64 at = 0; at < WRITTEN_BYTES; at += CHUNK_BYTES) {
        for (u64 index = 0; index < CHUNK_BYTES; index++) {
            u64 position = at + index;
            chunk[index] = (unsigned char)((position * 131U) ^ (position >> 12));
        }
        if (syscall3(SYS_WRITE, out, (s64)chunk, CHUNK_BYTES) != CHUNK_BYTES) {
            put("USBIO write error\n");
            finish(0);
        }
    }
    s64 synced = syscall1(SYS_FSYNC, out);
    (void)syscall1(SYS_CLOSE, out);
    elapsed = now_ns() - started;
    ms = elapsed / 1000000ULL;
    put("USBIO wrote ");
    put_number(WRITTEN_BYTES);
    put(" in ");
    put_number(ms);
    put(" ms, ");
    put_number(ms ? (u64)WRITTEN_BYTES * 1000ULL / ms / 1024ULL : 0);
    put(" KiB/s\n");
    finish(read_ok && synced == 0);
}

TUNIX_START(run)
