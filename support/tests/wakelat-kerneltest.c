typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#if defined(__x86_64__)
#define STACK_SLOT 8
#else
#define STACK_SLOT 16
#endif

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_PIPE 22
#define SYS_NANOSLEEP 35
#define SYS_PREAD64 17
#define SYS_FUTEX 202
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231

#define CLOCK_MONOTONIC 1
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define THREAD_FLAGS 0x10F00UL
#define SAMPLES 200U
#define SPINNERS 4U
#define IDLERS 1000U
#define HOG_BYTES (32UL << 20)
#define HOG_CHUNK (8UL << 20)

struct timespec {
    s64 sec;
    s64 nsec;
};

static char spin_stacks[SPINNERS][16384] __attribute__((aligned(16)));
static char idle_stacks[IDLERS][4096] __attribute__((aligned(16)));
static char helper_stack[65536] __attribute__((aligned(16)));
static char hog_buffers[SPINNERS][HOG_CHUNK] __attribute__((aligned(4096)));
static volatile unsigned spin_stop;
static volatile unsigned spinning[SPINNERS];
static volatile unsigned next_index;
static volatile unsigned idle_word;
static volatile unsigned idle_ready[IDLERS];
static volatile unsigned idle_timed;
static volatile unsigned ping;
static volatile u64 ping_sent;
static volatile unsigned helper_stop;
static volatile unsigned helper_running;
static int pipe_fds[2];
static int hog_fd = -1;
static u64 samples[SAMPLES];
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

static void spin_ns(u64 duration) {
    u64 until = now_ns() + duration;
    while (now_ns() < until) __asm__ volatile("" ::: "memory");
}

static void sort(u64 *values, unsigned count) {
    for (unsigned i = 1; i < count; i++) {
        u64 key = values[i];
        unsigned j = i;
        while (j && values[j - 1] > key) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = key;
    }
}

static void clear_samples(void) {
    for (unsigned index = 0; index < SAMPLES; index++) samples[index] = 0;
}

static void report(const char *name) {
    sort(samples, SAMPLES);
    put("WAKELAT ");
    put(name);
    put(" median_us=");
    put_number(samples[SAMPLES / 2] / 1000U);
    put(" p99_us=");
    put_number(samples[(SAMPLES * 99U) / 100U] / 1000U);
    put(" max_us=");
    put_number(samples[SAMPLES - 1] / 1000U);
    put("\n");
}

static int start(void (*entry)(void), char *stack, u64 size) {
    char *top = stack + size - STACK_SLOT;
    *(void **)top = (void *)entry;
    return spawn_thread(THREAD_FLAGS, top) < 0 ? -1 : 0;
}

static void spinner(void) {
    unsigned self = next_index;
    spinning[self] = 1;
    while (!spin_stop) __asm__ volatile("" ::: "memory");
    spinning[self] = 0;
}

static void hog(void) {
    unsigned self = next_index;
    spinning[self] = 1;
    while (!spin_stop) {
        for (u64 offset = 0; offset < HOG_BYTES && !spin_stop; offset += HOG_CHUNK)
            (void)syscall4(SYS_PREAD64, hog_fd, (s64)hog_buffers[self], (s64)HOG_CHUNK, (s64)offset);
    }
    spinning[self] = 0;
}

static void start_load(void (*entry)(void)) {
    spin_stop = 0;
    for (unsigned index = 0; index < SPINNERS; index++) {
        next_index = index;
        if (start(entry, spin_stacks[index], sizeof(spin_stacks[index])) != 0) {
            put("WAKELAT cannot start load\n");
            failures++;
            return;
        }
        while (!spinning[index]) sleep_ns(1000000UL);
    }
}

static void stop_load(void) {
    spin_stop = 1;
    for (unsigned index = 0; index < SPINNERS; index++)
        while (spinning[index]) sleep_ns(1000000UL);
}

static void futex_sleeper(void) {
    helper_running = 1;
    while (!helper_stop) {
        unsigned seen = ping;
        while (ping == seen && !helper_stop)
            (void)syscall4(SYS_FUTEX, (s64)&ping, FUTEX_WAIT_PRIVATE, (s64)seen, 0);
        if (helper_stop) break;
        u64 woke = now_ns();
        samples[ping - 1U] = woke - ping_sent;
    }
    helper_running = 0;
}

static void measure_futex_wake(const char *name) {
    clear_samples();
    ping = 0;
    helper_stop = 0;
    if (start(futex_sleeper, helper_stack, sizeof(helper_stack)) != 0) {
        failures++;
        return;
    }
    while (!helper_running) sleep_ns(1000000UL);
    for (unsigned index = 0; index < SAMPLES; index++) {
        spin_ns(2000000UL);
        ping_sent = now_ns();
        ping = index + 1U;
        (void)syscall3(SYS_FUTEX, (s64)&ping, FUTEX_WAKE_PRIVATE, 1);
    }
    sleep_ns(20000000UL);
    helper_stop = 1;
    (void)syscall3(SYS_FUTEX, (s64)&ping, FUTEX_WAKE_PRIVATE, 1);
    while (helper_running) sleep_ns(1000000UL);
    report(name);
}

static void pipe_reader(void) {
    helper_running = 1;
    for (unsigned index = 0; index < SAMPLES; index++) {
        char byte;
        (void)syscall3(SYS_READ, pipe_fds[0], (s64)&byte, 1);
        samples[index] = now_ns() - ping_sent;
    }
    helper_running = 0;
}

static void measure_pipe_wake(const char *name) {
    if (start(pipe_reader, helper_stack, sizeof(helper_stack)) != 0) {
        failures++;
        return;
    }
    while (!helper_running) sleep_ns(1000000UL);
    for (unsigned index = 0; index < SAMPLES; index++) {
        spin_ns(2000000UL);
        ping_sent = now_ns();
        char byte = 1;
        (void)syscall3(SYS_WRITE, pipe_fds[1], (s64)&byte, 1);
    }
    while (helper_running) sleep_ns(1000000UL);
    report(name);
}

static void measure_nanosleep(const char *name) {
    for (unsigned index = 0; index < SAMPLES; index++) {
        u64 start_time = now_ns();
        sleep_ns(1000000UL);
        u64 elapsed = now_ns() - start_time;
        samples[index] = elapsed > 1000000UL ? elapsed - 1000000UL : 0;
    }
    report(name);
}

static void idler(void) {
    unsigned self = next_index;
    idle_ready[self] = 1;
    struct timespec far = {30, 0};
    while (!spin_stop)
        (void)syscall4(SYS_FUTEX, (s64)&idle_word, FUTEX_WAIT_PRIVATE, 0,
                       idle_timed ? (s64)&far : 0);
}

static void start_idlers(void) {
    for (unsigned index = 0; index < IDLERS; index++) {
        next_index = index;
        if (start(idler, idle_stacks[index], sizeof(idle_stacks[index])) != 0) {
            put("WAKELAT cannot start idlers at ");
            put_number(index);
            put("\n");
            failures++;
            return;
        }
        while (!idle_ready[index]) sleep_ns(100000UL);
    }
}

static void make_hog_file(void) {
    hog_fd = (int)syscall3(SYS_OPEN, (s64)"/tmp/hog", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (hog_fd < 0) {
        put("WAKELAT cannot create /tmp/hog\n");
        failures++;
        return;
    }
    for (u64 index = 0; index < HOG_CHUNK; index++) hog_buffers[0][index] = (char)index;
    for (u64 written = 0; written < HOG_BYTES; written += HOG_CHUNK)
        (void)syscall3(SYS_WRITE, hog_fd, (s64)hog_buffers[0], (s64)HOG_CHUNK);
}

void run(void) {
    sleep_ns(1000000000UL);
    if (syscall1(SYS_PIPE, (s64)pipe_fds) != 0) failures++;
    measure_futex_wake("futex_wake_idle");
    start_load(spinner);
    measure_futex_wake("futex_wake_busy");
    measure_pipe_wake("pipe_wake_busy");
    stop_load();
    make_hog_file();
    start_load(hog);
    measure_nanosleep("nanosleep_1ms_syscall_hogs");
    measure_futex_wake("futex_wake_syscall_hogs");
    stop_load();
    idle_timed = 1;
    spin_stop = 0;
    start_idlers();
    measure_nanosleep("nanosleep_1ms_1000_timed_sleepers");
    u64 begin = now_ns();
    for (unsigned index = 0; index < SAMPLES; index++) sleep_ns(100000UL);
    put("WAKELAT 200_sleeps_of_100us_with_1000_sleepers_total_us=");
    put_number((now_ns() - begin) / 1000U);
    put("\n");
    put(failures ? "WAKELAT FAIL\n" : "WAKELAT PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
