typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_MMAP 9
#define SYS_MUNMAP 11
#define SYS_PIPE 22
#define SYS_DUP 32
#define SYS_GETPID 39
#define SYS_SOCKETPAIR 53
#define SYS_FORK 57
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_UNLINK 87
#define SYS_SYSLOG 103
#define SYS_EXIT_GROUP 231

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define AF_UNIX 1
#define SOCK_STREAM 1

#define THREAD_FLAGS 0x10F00UL
#define WORKERS 4U
#define ROUNDS 600U
#define FORK_EVERY 50U

static char stacks[WORKERS][65536] __attribute__((aligned(16)));
static volatile unsigned started;
static volatile unsigned finished;
static volatile unsigned go;
static volatile unsigned failures;
static volatile unsigned failed_step;

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

static void fail(unsigned step) {
    __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&failed_step, step, __ATOMIC_RELAXED);
}

static int same(const char *a, const char *b, u64 length) {
    for (u64 index = 0; index < length; index++)
        if (a[index] != b[index]) return 0;
    return 1;
}

static void fill(char *buffer, u64 length, unsigned seed) {
    for (u64 index = 0; index < length; index++)
        buffer[index] = (char)('a' + (seed + index) % 26U);
}

static void round_pipe(unsigned seed) {
    int fds[2];
    if (syscall1(SYS_PIPE, (s64)fds) != 0) { fail(1); return; }
    char out[512], in[512];
    fill(out, sizeof(out), seed);
    if (syscall3(SYS_WRITE, fds[1], (s64)out, sizeof(out)) != (s64)sizeof(out)) fail(2);
    if (syscall3(SYS_READ, fds[0], (s64)in, sizeof(in)) != (s64)sizeof(in)) fail(3);
    else if (!same(out, in, sizeof(in))) fail(4);
    (void)syscall1(SYS_CLOSE, fds[0]);
    (void)syscall1(SYS_CLOSE, fds[1]);
}

static void round_socket(unsigned seed) {
    int fds[2];
    if (syscall4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (s64)fds) != 0) { fail(5); return; }
    char out[300], in[300];
    fill(out, sizeof(out), seed * 7U);
    if (syscall3(SYS_WRITE, fds[0], (s64)out, sizeof(out)) != (s64)sizeof(out)) fail(6);
    if (syscall3(SYS_READ, fds[1], (s64)in, sizeof(in)) != (s64)sizeof(in)) fail(7);
    else if (!same(out, in, sizeof(in))) fail(8);
    (void)syscall1(SYS_CLOSE, fds[0]);
    (void)syscall1(SYS_CLOSE, fds[1]);
}

static void round_file(unsigned worker, unsigned seed) {
    char path[] = "/tmp/stress-0";
    path[12] = (char)('0' + worker);
    int fd = (int)syscall3(SYS_OPEN, (s64)path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { fail(9); return; }
    char out[1024], in[1024];
    fill(out, sizeof(out), seed * 3U);
    if (syscall3(SYS_WRITE, fd, (s64)out, sizeof(out)) != (s64)sizeof(out)) fail(10);
    (void)syscall1(SYS_CLOSE, fd);
    fd = (int)syscall3(SYS_OPEN, (s64)path, O_RDONLY, 0);
    if (fd < 0) { fail(11); return; }
    if (syscall3(SYS_READ, fd, (s64)in, sizeof(in)) != (s64)sizeof(in)) fail(12);
    else if (!same(out, in, sizeof(in))) fail(13);
    int copy = (int)syscall1(SYS_DUP, fd);
    if (copy < 0) fail(14);
    else (void)syscall1(SYS_CLOSE, copy);
    (void)syscall1(SYS_CLOSE, fd);
    if (seed % 4U == 0 && syscall1(SYS_UNLINK, (s64)path) != 0) fail(15);
}

static void round_proc(void) {
    int fd = (int)syscall3(SYS_OPEN, (s64)"/proc/self/stat", O_RDONLY, 0);
    if (fd < 0) { fail(16); return; }
    char text[512];
    if (syscall3(SYS_READ, fd, (s64)text, sizeof(text)) <= 0) fail(17);
    (void)syscall1(SYS_CLOSE, fd);
}

static void round_memory(unsigned seed) {
    u64 length = 16U * 4096U;
    s64 base = syscall6(SYS_MMAP, 0, (s64)length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base < 0 && base > -4096) { fail(18); return; }
    volatile char *bytes = (volatile char *)base;
    for (u64 offset = 0; offset < length; offset += 4096U) bytes[offset] = (char)seed;
    for (u64 offset = 0; offset < length; offset += 4096U)
        if (bytes[offset] != (char)seed) { fail(19); break; }
    if (syscall2(SYS_MUNMAP, base, (s64)length) != 0) fail(20);
}

static void round_fork(void) {
    s64 child = syscall0(SYS_FORK);
    if (child == 0) {
        (void)syscall0(SYS_GETPID);
        syscall1(SYS_EXIT, 7);
        for (;;) { }
    }
    if (child < 0) { fail(21); return; }
    int status = 0;
    if (syscall4(SYS_WAIT4, child, (s64)&status, 0, 0) != child) fail(22);
    else if (((status >> 8) & 0xFF) != 7) fail(23);
}

static void work(unsigned worker) {
    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned seed = worker * 100003U + round;
        round_pipe(seed);
        round_socket(seed);
        round_file(worker, seed);
        round_proc();
        round_memory(seed);
        if (round % FORK_EVERY == worker) round_fork();
    }
}

static void worker_body(void) {
    unsigned worker = __atomic_add_fetch(&started, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) { }
    work(worker);
    __atomic_fetch_add(&finished, 1, __ATOMIC_RELEASE);
}

static int kernel_log_clean(void) {
    static char log[65536];
    s64 got = syscall3(SYS_SYSLOG, 3, (s64)log, sizeof(log));
    if (got < 0) return 1;
    for (s64 at = 0; at + 10 <= got; at++) {
        if (same(log + at, "LOCK: cpu ", 10)) {
            s64 end = at;
            while (end < got && log[end] != '\n') end++;
            (void)syscall3(SYS_WRITE, 1, (s64)(log + at), end - at + 1);
            return 0;
        }
    }
    return 1;
}

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    unsigned helpers = 0;
    for (unsigned index = 1; index < WORKERS; index++) {
        char *top = stacks[index] + sizeof(stacks[index]) - 8;
        *(void **)top = (void *)worker_body;
        if (spawn_thread(THREAD_FLAGS, top) < 0) break;
        helpers++;
    }
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) < helpers) { }
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    work(0);
    while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < helpers) { }

    int clean = kernel_log_clean();
    put("SMPSTRESS workers=");
    put_number(helpers + 1);
    put(" rounds=");
    put_number(ROUNDS);
    put(" failures=");
    put_number(failures);
    put(" step=");
    put_number(failed_step);
    put(clean ? " log=clean\n" : " log=warnings\n");
    put(failures == 0 && clean ? "SMPSTRESS PASS\n" : "SMPSTRESS FAIL\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
