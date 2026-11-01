#define TEST_NAME "kcov"
#include "test.h"

#define KCOV_INIT_TRACE 0x80086301UL
#define KCOV_ENABLE     0x6364UL
#define KCOV_DISABLE    0x6365UL
#define KCOV_RESET      0x6368UL
#define KCOV_TRACE_PC   0
#define KCOV_TRACE_CMP  1
#define KERNEL_SPACE    0xFFFF000000000000UL
#define WORDS           (64UL * 1024UL)
#define EINVAL          22
#define EBUSY           16
#define ENOTTY          25

static s64 kcov_ioctl(int fd, u64 request, u64 argument) {
    return SYSCALL(SYS_ioctl, fd, request, argument, 0, 0, 0);
}

static int open_kcov(void) { return (int)open("/sys/kernel/debug/kcov", O_RDWR, 0); }

static u64 *map_area(int fd, u64 words) {
    void *area = mmap_file(words * 8U, PROT_READ | PROT_WRITE, fd, 0);
    return (s64)area < 0 ? (u64 *)0 : (u64 *)area;
}

static int all_kernel(const u64 *area, u64 first, u64 count, u64 stride) {
    for (u64 index = 0; index < count; index++)
        if (area[first + index * stride] < KERNEL_SPACE) return 0;
    return 1;
}

static void tracing_pcs(void) {
    int fd = open_kcov();
    expect(fd >= 0, "/sys/kernel/debug/kcov opens");
    expect_eq(kcov_ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC), -EINVAL,
              "it cannot be enabled before it has a buffer");
    expect_eq(kcov_ioctl(fd, KCOV_INIT_TRACE, 1), -EINVAL, "a one-word buffer is refused");
    expect_eq(kcov_ioctl(fd, KCOV_INIT_TRACE, WORDS), 0, "a 64k-word buffer is accepted");
    expect_eq(kcov_ioctl(fd, KCOV_INIT_TRACE, WORDS), -EBUSY, "and cannot be set up twice");
    expect((s64)mmap_file((WORDS + 1024U) * 8U, PROT_READ | PROT_WRITE, fd, 0) < 0,
           "a mapping larger than the buffer is refused");
    u64 *area = map_area(fd, WORDS);
    expect(area != 0, "the buffer maps shared");
    if (!area) return;
    expect_eq(kcov_ioctl(fd, KCOV_ENABLE, 7), -EINVAL, "an unknown mode is refused");
    expect_eq(kcov_ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC), 0, "tracing starts");
    expect_eq(kcov_ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC), -EBUSY, "and cannot start twice");
    __atomic_store_n(&area[0], 0, __ATOMIC_RELAXED);
    getpid();
    u64 getpid_pcs = __atomic_load_n(&area[0], __ATOMIC_RELAXED);
    expect(getpid_pcs > 0 && getpid_pcs < WORDS, "getpid records kernel coverage");
    expect(all_kernel(area, 1, getpid_pcs, 1), "every recorded pc is a kernel address");
    __atomic_store_n(&area[0], 0, __ATOMIC_RELAXED);
    int proc = (int)open("/proc/self/status", O_RDONLY, 0);
    char buffer[512];
    read(proc, buffer, sizeof(buffer));
    close(proc);
    u64 file_pcs = __atomic_load_n(&area[0], __ATOMIC_RELAXED);
    expect(file_pcs > getpid_pcs, "opening and reading a file covers more than getpid");
    s64 reset = kcov_ioctl(fd, KCOV_RESET, 0);
    u64 after_reset = __atomic_load_n(&area[0], __ATOMIC_RELAXED);
    expect_eq(reset, 0, "the trace can be reset by ioctl");
    expect(after_reset < getpid_pcs + 64U, "which empties it");
    expect_eq(kcov_ioctl(fd, 0x6399, 0), -ENOTTY, "an unknown request is ENOTTY");
    expect_eq(kcov_ioctl(fd, KCOV_DISABLE, 0), 0, "tracing stops");
    __atomic_store_n(&area[0], 0, __ATOMIC_RELAXED);
    getpid();
    expect_eq((s64)__atomic_load_n(&area[0], __ATOMIC_RELAXED), 0,
              "nothing is recorded once it is disabled");
    expect_eq(kcov_ioctl(fd, KCOV_DISABLE, 0), -EINVAL, "disabling twice is refused");
    munmap(area, WORDS * 8U);
    close(fd);
}

static void comparisons(void) {
    int fd = open_kcov();
    kcov_ioctl(fd, KCOV_INIT_TRACE, WORDS);
    u64 *area = map_area(fd, WORDS);
    if (!area) {
        expect(0, "the comparison buffer maps");
        return;
    }
    expect_eq(kcov_ioctl(fd, KCOV_ENABLE, KCOV_TRACE_CMP), 0, "comparison tracing starts");
    __atomic_store_n(&area[0], 0, __ATOMIC_RELAXED);
    int proc = (int)open("/proc/self/status", O_RDONLY, 0);
    close(proc);
    u64 records = __atomic_load_n(&area[0], __ATOMIC_RELAXED);
    kcov_ioctl(fd, KCOV_DISABLE, 0);
    expect(records > 0 && records * 4U + 1U <= WORDS, "an open records comparisons");
    int typed = 1;
    for (u64 index = 0; index < records; index++) typed &= area[1U + index * 4U] < 8U;
    expect(typed, "every record carries a size and constant flag only");
    expect(all_kernel(area, 4, records, 4), "and the pc of a kernel comparison");
    munmap(area, WORDS * 8U);
    close(fd);
}

static void overflow(void) {
    int fd = open_kcov();
    kcov_ioctl(fd, KCOV_INIT_TRACE, 16);
    u64 *area = map_area(fd, 16);
    if (!area) {
        expect(0, "a small buffer maps");
        return;
    }
    kcov_ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC);
    __atomic_store_n(&area[0], 0, __ATOMIC_RELAXED);
    for (int index = 0; index < 20; index++) getpid();
    u64 count = __atomic_load_n(&area[0], __ATOMIC_RELAXED);
    kcov_ioctl(fd, KCOV_DISABLE, 0);
    expect_eq((s64)count, 15, "a full buffer stops at its last word");
    munmap(area, 16U * 8U);
    close(fd);
}

static void lifetimes(void) {
    int fd = open_kcov();
    kcov_ioctl(fd, KCOV_INIT_TRACE, WORDS);
    u64 *area = map_area(fd, WORDS);
    kcov_ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC);
    s64 child = fork();
    if (child == 0) {
        s64 status = kcov_ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC);
        exit(status == -EBUSY ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    kcov_ioctl(fd, KCOV_DISABLE, 0);
    expect_eq(exited_with(status), 0, "a child cannot take over its parent's trace");

    close(fd);
    expect(area && __atomic_load_n(&area[0], __ATOMIC_RELAXED) < WORDS,
           "the mapping outlives the descriptor");
    if (area) {
        area[1] = 1;
        munmap(area, WORDS * 8U);
    }

    child = fork();
    if (child == 0) {
        int own = open_kcov();
        kcov_ioctl(own, KCOV_INIT_TRACE, WORDS);
        u64 *mine = map_area(own, WORDS);
        kcov_ioctl(own, KCOV_ENABLE, KCOV_TRACE_PC);
        if (mine) mine[0] = 0;
        getpid();
        exit(mine && mine[0] > 0 ? 0 : 1);
    }
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "a process may exit while it is still tracing");

    child = fork();
    if (child == 0) {
        int closer = open_kcov();
        kcov_ioctl(closer, KCOV_INIT_TRACE, WORDS);
        u64 *kept = map_area(closer, WORDS);
        kcov_ioctl(closer, KCOV_ENABLE, KCOV_TRACE_PC);
        close(closer);
        if (kept) kept[0] = 0;
        getpid();
        int still = kept && kept[0] > 0;
        int fresh = open_kcov();
        kcov_ioctl(fresh, KCOV_INIT_TRACE, 64);
        int busy = kcov_ioctl(fresh, KCOV_ENABLE, KCOV_TRACE_PC) == -EBUSY;
        exit(still && busy ? 0 : 1);
    }
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0,
              "closing the descriptor keeps the trace on until the task exits");

    int cycled = 1;
    for (int round = 0; round < 64; round++) {
        int again = open_kcov();
        cycled &= kcov_ioctl(again, KCOV_INIT_TRACE, 512U * 1024U) == 0;
        u64 *big = map_area(again, 512U * 1024U);
        cycled &= big != 0;
        cycled &= kcov_ioctl(again, KCOV_ENABLE, round & 1 ? KCOV_TRACE_CMP : KCOV_TRACE_PC) == 0;
        getpid();
        cycled &= kcov_ioctl(again, KCOV_DISABLE, 0) == 0;
        if (big) munmap(big, 512U * 1024U * 8U);
        close(again);
    }
    expect(cycled, "64 rounds of 4 MiB buffers are set up and released");
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    tracing_pcs();
    comparisons();
    overflow();
    lifetimes();
}
