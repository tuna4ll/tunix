#define TEST_NAME "swap"
#include "test.h"

#define SWAPFILE "/swapfile"
#define SWAP_MIB 768UL
#define CHUNK    (1UL << 20)
#define ETXTBSY  26
#define EPERM    1
#define EBUSY    16
#define EINVAL   22

static u8 chunk[CHUNK];

static u64 pattern(u64 page, u64 salt) {
    u64 value = (page + 1) * 0x9E3779B97F4A7C15UL ^ salt;
    value ^= value >> 29;
    return value;
}

static void fill(u8 *area, u64 pages, u64 salt) {
    for (u64 page = 0; page < pages; page++) {
        u64 *words = (u64 *)(area + page * PAGE_SIZE);
        words[0] = pattern(page, salt);
        words[257] = ~pattern(page, salt);
        words[511] = page;
    }
}

static u64 mismatches(const u8 *area, u64 pages, u64 salt) {
    u64 wrong = 0;
    for (u64 page = 0; page < pages; page++) {
        const u64 *words = (const u64 *)(area + page * PAGE_SIZE);
        if (words[0] != pattern(page, salt) || words[257] != ~pattern(page, salt) ||
            words[511] != page)
            wrong++;
    }
    return wrong;
}

static s64 field_kb(const char *path, const char *field) {
    static char text[4096];
    s64 fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    s64 length = read((int)fd, text, sizeof(text) - 1);
    close((int)fd);
    if (length <= 0) return -1;
    text[length] = 0;
    u64 wanted = strlen(field);
    for (s64 at = 0; at < length; at++) {
        if ((at && text[at - 1] != '\n') || !memeq(text + at, field, wanted)) continue;
        s64 value = 0;
        for (at += (s64)wanted; at < length && (text[at] < '0' || text[at] > '9'); at++) {}
        while (at < length && text[at] >= '0' && text[at] <= '9')
            value = value * 10 + text[at++] - '0';
        return value;
    }
    return -1;
}

static int contains(const char *path, const char *needle) {
    static char text[4096];
    s64 fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    s64 length = read((int)fd, text, sizeof(text) - 1);
    close((int)fd);
    if (length <= 0) return 0;
    text[length] = 0;
    u64 wanted = strlen(needle);
    for (s64 at = 0; at + (s64)wanted <= length; at++)
        if (memeq(text + at, needle, wanted)) return 1;
    return 0;
}

static int make_swapfile(void) {
    s64 fd = open(SWAPFILE, O_WRONLY | O_CREAT, 0600);
    if (fd < 0) return 0;
    for (u64 at = 0; at < CHUNK; at++) chunk[at] = 0;
    int ok = 1;
    for (u64 written = 0; ok && written < SWAP_MIB * CHUNK; written += CHUNK)
        ok = write((int)fd, chunk, CHUNK) == (s64)CHUNK;
    u32 *info = (u32 *)(chunk + 1024);
    info[0] = 1;
    info[1] = (u32)(SWAP_MIB * CHUNK / PAGE_SIZE - 1);
    info[2] = 0;
    const char magic[] = "SWAPSPACE2";
    for (u64 at = 0; at < 10; at++) chunk[4086 + at] = (u8)magic[at];
    ok = ok && lseek((int)fd, 0, 0) == 0 && write((int)fd, chunk, PAGE_SIZE) == (s64)PAGE_SIZE;
    ok = ok && fsync((int)fd) == 0;
    close((int)fd);
    return ok;
}

static void guard_checks(void) {
    s64 fd = open(SWAPFILE, O_WRONLY, 0);
    expect(fd >= 0, "the active swap file still opens");
    expect_eq(write((int)fd, chunk, 16), -ETXTBSY, "writing to an active swap file is refused");
    expect_eq(ftruncate((int)fd, 0), -ETXTBSY, "truncating it is refused");
    close((int)fd);
    expect_eq(unlink(SWAPFILE), -EPERM, "unlinking it is refused");
    expect_eq(swapon(SWAPFILE, 0), -EBUSY, "it cannot be activated twice");
    expect_eq(swapon("/proc/meminfo", 0), -EINVAL, "a file without a swap header is refused");
}

static void child_verifies(u8 *area, u64 pages) {
    u64 wrong = mismatches(area, pages, 1);
    fill(area, pages / 2, 2);
    exit(wrong == 0 && mismatches(area, pages / 2, 2) == 0 ? 0 : 1);
}

static void protected_write(volatile u8 *page) { page[8] = 1; }

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    struct sysinfo info;
    expect_eq(sysinfo(&info), 0, "sysinfo works");
    expect_eq((s64)info.totalswap, 0, "there is no swap at boot");

    expect(make_swapfile(), "a 768 MiB swap file is written and synced");
    expect_eq(swapon(SWAPFILE, 0), 0, "swapon accepts it");
    expect(contains("/proc/swaps", SWAPFILE), "/proc/swaps lists it");
    expect(contains("/proc/swaps", "file"), "as a file");
    expect_eq(sysinfo(&info), 0, "sysinfo still works");
    expect_eq((s64)info.totalswap, (s64)(SWAP_MIB * CHUNK - PAGE_SIZE),
              "sysinfo reports the swap size without the header page");
    expect_eq(field_kb("/proc/meminfo", "SwapTotal:"), (s64)(SWAP_MIB * 1024 - 4),
              "meminfo reports it too");
    guard_checks();

    u64 bytes = info.totalram + (128UL << 20);
    u64 pages = bytes / PAGE_SIZE;
    u8 *area = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    expect((s64)area > 0, "more anonymous memory than RAM is mapped");
    fill(area, pages, 1);
    expect(field_kb("/proc/self/status", "VmSwap:") > 0, "part of it is swapped out");
    expect(field_kb("/proc/meminfo", "SwapFree:") < (s64)(SWAP_MIB * 1024 - 4),
           "SwapFree goes down");
    expect_eq((s64)mismatches(area, pages, 1), 0, "every page reads back what was written");

    s64 child = fork();
    if (child == 0) child_verifies(area, pages);
    int status = -1;
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "a child sees the swapped pages and writes its own copies");
    expect_eq((s64)mismatches(area, pages, 1), 0, "the parent's pages are untouched by it");

    u64 shared_pages = (64UL << 20) / PAGE_SIZE;
    u8 *shared =
        mmap(0, shared_pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
    expect((s64)shared > 0, "a 64 MiB shared anonymous mapping is created");
    fill(shared, shared_pages, 3);
    child = fork();
    if (child == 0) {
        u64 wrong = mismatches(shared, shared_pages, 3);
        fill(area, pages / 4, 4);
        fill(shared, shared_pages, 5);
        exit(wrong == 0 ? 0 : 1);
    }
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "the child reads the shared pages under pressure");
    expect_eq((s64)mismatches(shared, shared_pages, 5), 0,
              "the parent sees every shared page the child rewrote");
    expect_eq((s64)mismatches(area, pages, 1), 0, "and its private pages are still its own");

    u64 locked = pages / 4;
    expect_eq(mprotect(area, locked * PAGE_SIZE, PROT_READ), 0, "mprotect works on swapped pages");
    child = fork();
    if (child == 0) {
        protected_write(area + (locked / 2) * PAGE_SIZE);
        exit(0);
    }
    waitpid(child, &status, 0);
    expect_eq(killed_by(status), SIGSEGV, "a swapped page made read-only refuses writes");
    expect_eq((s64)mismatches(area, locked, 1), 0, "and still reads correctly");

    s64 free_before = field_kb("/proc/meminfo", "SwapFree:");
    u64 keep = (64UL << 20) / PAGE_SIZE;
    expect_eq(munmap(area + keep * PAGE_SIZE, (pages - keep) * PAGE_SIZE), 0,
              "most of the area is unmapped");
    expect(field_kb("/proc/meminfo", "SwapFree:") > free_before, "its swap slots are released");
    expect_eq(munmap(shared, shared_pages * PAGE_SIZE), 0, "the shared mapping is unmapped");

    expect_eq(swapoff(SWAPFILE), 0, "swapoff brings every page back");
    expect(!contains("/proc/swaps", SWAPFILE), "/proc/swaps no longer lists it");
    expect_eq(field_kb("/proc/self/status", "VmSwap:"), 0, "nothing is swapped any more");
    expect_eq((s64)mismatches(area, keep, 1), 0, "the remaining pages survived the round trip");
    expect_eq(swapoff(SWAPFILE), -EINVAL, "a second swapoff is refused");
    munmap(area, keep * PAGE_SIZE);
    expect_eq(unlink(SWAPFILE), 0, "the swap file can be removed afterwards");
}
