typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_NANOSLEEP 35
#define SYS_FSYNC 74
#define SYS_MKDIR 83
#define SYS_UNLINK 87
#define SYS_SYNC 162
#define SYS_MOUNT 165
#define SYS_UMOUNT2 166
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231

#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define CLOCK_MONOTONIC 1

#define THREAD_FLAGS 0x10F00UL
#define WRITE_BYTES (24UL * 1024UL * 1024UL)
#define CHUNK_BYTES (64UL * 1024UL)
#define SLEEP_NS 5000000UL
#define TIMER_LIMIT_NS 100000000UL
#define FS_LIMIT_NS 250000000UL

struct timespec {
    s64 seconds;
    s64 nanoseconds;
};

static char stacks[2][65536] __attribute__((aligned(16)));
static char chunk[CHUNK_BYTES];
static char back[CHUNK_BYTES];
static volatile unsigned stop;
static volatile unsigned finished;
static volatile u64 timer_worst;
static volatile u64 timer_rounds;
static volatile u64 fs_worst;
static volatile u64 fs_rounds;
static volatile unsigned fs_errors;

static u64 now_ns(void) {
    struct timespec value;
    (void)syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.seconds * 1000000000UL + (u64)value.nanoseconds;
}

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

static void put_ms(const char *label, u64 nanoseconds) {
    put(label);
    put_number(nanoseconds / 1000000UL);
    put(".");
    u64 fraction = (nanoseconds / 1000UL) % 1000UL;
    if (fraction < 100) put("0");
    if (fraction < 10) put("0");
    put_number(fraction);
}

static void timer_probe(void) {
    struct timespec pause = {0, SLEEP_NS};
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        u64 before = now_ns();
        (void)syscall2(SYS_NANOSLEEP, (s64)&pause, 0);
        u64 late = now_ns() - before;
        late = late > SLEEP_NS ? late - SLEEP_NS : 0;
        if (late > timer_worst) timer_worst = late;
        timer_rounds++;
    }
    __atomic_fetch_add(&finished, 1, __ATOMIC_RELEASE);
}

static void fs_probe(void) {
    struct timespec pause = {0, SLEEP_NS};
    char buffer[4096];
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        u64 before = now_ns();
        int fd = (int)syscall3(SYS_OPEN, (s64)"/sbin/init", O_RDONLY, 0);
        if (fd < 0 || syscall3(SYS_READ, fd, (s64)buffer, sizeof(buffer)) <= 0) fs_errors++;
        if (fd >= 0) (void)syscall1(SYS_CLOSE, fd);
        u64 taken = now_ns() - before;
        if (taken > fs_worst) fs_worst = taken;
        fs_rounds++;
        (void)syscall2(SYS_NANOSLEEP, (s64)&pause, 0);
    }
    __atomic_fetch_add(&finished, 1, __ATOMIC_RELEASE);
}

static void start(void (*body)(void), unsigned slot) {
    char *top = stacks[slot] + sizeof(stacks[slot]) - 8;
    *(void **)top = (void *)body;
    if (spawn_thread(THREAD_FLAGS, top) < 0) {
        put("IOLAT cannot start a probe\n");
        __atomic_fetch_add(&finished, 1, __ATOMIC_RELEASE);
    }
}

static void fill(u64 offset) {
    for (u64 index = 0; index < CHUNK_BYTES; index++)
        chunk[index] = (char)((offset + index) * 131U >> 3);
}

static int same(const char *a, const char *b, u64 length) {
    for (u64 index = 0; index < length; index++)
        if (a[index] != b[index]) return 0;
    return 1;
}

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    (void)syscall2(SYS_MKDIR, (s64)"/stick", 0755);
    s64 mounted = syscall6(SYS_MOUNT, (s64)"/dev/sdb", (s64)"/stick", (s64)"ext3", 0, 0, 0);
    if (mounted != 0) {
        put("IOLAT cannot mount /dev/sdb\nIOLAT FAIL\n");
        syscall1(SYS_EXIT_GROUP, 1);
    }
    int warm = (int)syscall3(SYS_OPEN, (s64)"/sbin/init", O_RDONLY, 0);
    if (warm >= 0) {
        while (syscall3(SYS_READ, warm, (s64)back, sizeof(back)) > 0) { }
        (void)syscall1(SYS_CLOSE, warm);
    }

    start(timer_probe, 0);
    start(fs_probe, 1);

    int fd = (int)syscall3(SYS_OPEN, (s64)"/stick/big", O_RDWR | O_CREAT | O_TRUNC, 0644);
    unsigned failures = fd < 0;
    u64 begun = now_ns();
    for (u64 offset = 0; fd >= 0 && offset < WRITE_BYTES; offset += CHUNK_BYTES) {
        fill(offset);
        if (syscall3(SYS_WRITE, fd, (s64)chunk, CHUNK_BYTES) != (s64)CHUNK_BYTES) {
            put("IOLAT write failed\n");
            failures++;
            break;
        }
    }
    u64 written = now_ns();
    if (fd >= 0 && syscall1(SYS_FSYNC, fd) != 0) {
        put("IOLAT fsync failed\n");
        failures++;
    }
    u64 synced = now_ns();
    if (fd >= 0) (void)syscall1(SYS_CLOSE, fd);

    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < 2) { }

    fd = (int)syscall3(SYS_OPEN, (s64)"/stick/big", O_RDONLY, 0);
    for (u64 offset = 0; fd >= 0 && offset < WRITE_BYTES; offset += CHUNK_BYTES) {
        fill(offset);
        if (syscall3(SYS_READ, fd, (s64)back, CHUNK_BYTES) != (s64)CHUNK_BYTES ||
            !same(chunk, back, CHUNK_BYTES)) {
            put("IOLAT read back differs at ");
            put_number(offset);
            put("\n");
            failures++;
            break;
        }
    }
    if (fd >= 0) (void)syscall1(SYS_CLOSE, fd);
    else failures++;
    s64 unmounted = syscall2(SYS_UMOUNT2, (s64)"/stick", 0);
    if (unmounted != 0) {
        put("IOLAT umount failed ");
        put_number((u64)-unmounted);
        put("\n");
        failures++;
    }

    put_ms("IOLAT write_ms=", written - begun);
    put_ms(" fsync_ms=", synced - written);
    put_ms(" timer_worst_ms=", timer_worst);
    put(" timer_rounds=");
    put_number(timer_rounds);
    put_ms(" fs_worst_ms=", fs_worst);
    put(" fs_rounds=");
    put_number(fs_rounds);
    put(" fs_errors=");
    put_number(fs_errors);
    put(" data_errors=");
    put_number(failures);
    put("\n");
    int passed = !failures && !fs_errors && timer_worst < TIMER_LIMIT_NS &&
                 fs_worst < FS_LIMIT_NS;
    put(passed ? "IOLAT PASS\n" : "IOLAT FAIL\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
