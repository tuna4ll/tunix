typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_NANOSLEEP 35
#define SYS_PREAD64 17
#define SYS_UNLINK 87
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231
#define SYS_NEWFSTATAT 262

#define CLOCK_MONOTONIC 1
#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define AT_FDCWD -100
#define THREAD_FLAGS 0x10F00UL
#define MAX_SAMPLES 20000U
#define CHUNK (256UL << 10)

struct timespec {
    s64 sec;
    s64 nsec;
};

static char reader_stack[65536] __attribute__((aligned(16)));
static char cold_buffer[CHUNK] __attribute__((aligned(4096)));
static char stat_buffer[256];
static volatile unsigned reading;
static volatile unsigned done_reading;
static volatile u64 cold_bytes;
static volatile u64 cold_ns;
static u64 samples[MAX_SAMPLES];
static unsigned failures;

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
    struct timespec value;
    (void)syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.sec * 1000000000UL + (u64)value.nsec;
}

static void sleep_ns(u64 duration) {
    struct timespec request = {(s64)(duration / 1000000000UL), (s64)(duration % 1000000000UL)};
    (void)syscall2(SYS_NANOSLEEP, (s64)&request, 0);
}

static void sort(u64 *values, unsigned count) {
    for (unsigned gap = count / 2; gap; gap /= 2)
        for (unsigned i = gap; i < count; i++) {
            u64 key = values[i];
            unsigned j = i;
            while (j >= gap && values[j - gap] > key) {
                values[j] = values[j - gap];
                j -= gap;
            }
            values[j] = key;
        }
}

static void cold_reader(void) {
    int fd = (int)syscall3(SYS_OPEN, (s64)"/cold.bin", O_RDONLY, 0);
    reading = 1;
    if (fd < 0) {
        done_reading = 1;
        return;
    }
    u64 begin = now_ns();
    u64 total = 0;
    for (;;) {
        s64 got = syscall3(SYS_READ, fd, (s64)cold_buffer, (s64)CHUNK);
        if (got <= 0) break;
        total += (u64)got;
    }
    cold_ns = now_ns() - begin;
    cold_bytes = total;
    (void)syscall1(SYS_CLOSE, fd);
    done_reading = 1;
}

static u64 warm_operation(unsigned round) {
    u64 begin = now_ns();
    (void)syscall4(SYS_NEWFSTATAT, AT_FDCWD, (s64)"/sbin/init", (s64)stat_buffer, 0);
    int fd = (int)syscall3(SYS_OPEN, (s64)"/sbin/init", O_RDONLY, 0);
    if (fd >= 0) {
        char small[512];
        (void)syscall4(SYS_PREAD64, fd, (s64)small, sizeof(small), 0);
        (void)syscall1(SYS_CLOSE, fd);
    }
    if ((round & 7U) == 0) {
        int created = (int)syscall3(SYS_OPEN, (s64)"/tmp/probe", O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (created >= 0) {
            (void)syscall3(SYS_WRITE, created, (s64)"x", 1);
            (void)syscall1(SYS_CLOSE, created);
        }
        (void)syscall1(SYS_UNLINK, (s64)"/tmp/probe");
    }
    return now_ns() - begin;
}

static void report(const char *name, unsigned count) {
    if (!count) {
        put("VFSLAT no samples\n");
        failures++;
        return;
    }
    sort(samples, count);
    put("VFSLAT ");
    put(name);
    put(" ops=");
    put_number(count);
    put(" median_us=");
    put_number(samples[count / 2] / 1000U);
    put(" p99_us=");
    put_number(samples[(count * 99U) / 100U] / 1000U);
    put(" max_us=");
    put_number(samples[count - 1] / 1000U);
    put("\n");
}

void run(void) {
    unsigned count = 0;
    for (unsigned round = 0; round < 2000U; round++) samples[count++] = warm_operation(round);
    report("quiet", count);

    char *top = reader_stack + sizeof(reader_stack) - 16;
    *(void **)top = (void *)cold_reader;
    if (spawn_thread(THREAD_FLAGS, top) < 0) {
        put("VFSLAT FAIL\n");
        syscall1(SYS_EXIT_GROUP, 1);
    }
    while (!reading) sleep_ns(100000UL);
    count = 0;
    for (unsigned round = 0; !done_reading && count < MAX_SAMPLES; round++) {
        samples[count++] = warm_operation(round);
        sleep_ns(200000UL);
    }
    report("during_cold_read", count);
    put("VFSLAT cold_read_mib_per_s=");
    put_number(cold_ns ? (cold_bytes * 1000UL) / cold_ns : 0);
    put(" bytes=");
    put_number(cold_bytes);
    put("\n");
    if (!cold_bytes) failures++;
    put(failures ? "VFSLAT FAIL\n" : "VFSLAT PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
