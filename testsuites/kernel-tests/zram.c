#define TEST_NAME "zram"
#include "test.h"

#define DEVICE   "/dev/zram0"
#define SYSFS    "/sys/block/zram0/"
#define DISK_MIB 512UL
#define EBUSY    16
#define PREFER   0x8000

static u8 page_buffer[3 * PAGE_SIZE];
static u8 check_buffer[3 * PAGE_SIZE];

static int read_text(const char *path, char *text, u64 room) {
    s64 fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    s64 length = read((int)fd, text, room - 1);
    close((int)fd);
    if (length < 0) return -1;
    text[length] = 0;
    return 0;
}

static s64 write_text(const char *path, const char *text) {
    s64 fd = open(path, O_WRONLY, 0);
    if (fd < 0) return fd;
    s64 written = write((int)fd, text, strlen(text));
    close((int)fd);
    return written;
}

static u64 number_at(const char *text, unsigned field) {
    u64 value = 0;
    unsigned seen = 0;
    for (const char *at = text; *at; at++) {
        if (*at >= '0' && *at <= '9') {
            value = value * 10 + (u64)(*at - '0');
            continue;
        }
        if (at > text && at[-1] >= '0' && at[-1] <= '9') {
            if (seen == field) return value;
            seen++;
            value = 0;
        }
    }
    return seen == field ? value : 0;
}

static u64 sysfs_number(const char *name, unsigned field) {
    char path[64] = SYSFS;
    u64 at = strlen(path);
    for (u64 index = 0; name[index]; index++) path[at++] = name[index];
    path[at] = 0;
    char text[256];
    if (read_text(path, text, sizeof(text)) != 0) return (u64)-1;
    return number_at(text, field);
}

static u64 pattern(u64 page, u64 salt) {
    u64 value = (page + 1) * 0x9E3779B97F4A7C15UL ^ salt;
    return value ^ (value >> 31);
}

static void fill(u8 *area, u64 pages, u64 salt) {
    for (u64 page = 0; page < pages; page++) {
        u64 *words = (u64 *)(area + page * PAGE_SIZE);
        words[0] = pattern(page, salt);
        words[300] = ~pattern(page, salt);
        words[511] = page;
    }
}

static u64 mismatches(const u8 *area, u64 pages, u64 salt) {
    u64 wrong = 0;
    for (u64 page = 0; page < pages; page++) {
        const u64 *words = (const u64 *)(area + page * PAGE_SIZE);
        if (words[0] != pattern(page, salt) || words[300] != ~pattern(page, salt) ||
            words[511] != page)
            wrong++;
    }
    return wrong;
}

static void block_io(void) {
    s64 fd = open(DEVICE, O_RDWR, 0);
    expect(fd >= 0, "/dev/zram0 opens for reading and writing");
    u64 state = 0x12345678;
    for (u64 at = 0; at < sizeof(page_buffer); at++) {
        state = state * 6364136223846793005UL + 1442695040888963407UL;
        page_buffer[at] = (u8)(state >> 56);
    }
    expect_eq(pread((int)fd, check_buffer, PAGE_SIZE, 8 * PAGE_SIZE), (s64)PAGE_SIZE,
              "an untouched page reads");
    int zero = 1;
    for (u64 at = 0; at < PAGE_SIZE; at++) zero &= check_buffer[at] == 0;
    expect(zero, "and reads as zeros");

    expect_eq(lseek((int)fd, 4 * PAGE_SIZE, 0), (s64)(4 * PAGE_SIZE), "seeking works");
    expect_eq(write((int)fd, page_buffer, sizeof(page_buffer)), (s64)sizeof(page_buffer),
              "three pages of random bytes are written");
    expect_eq(pread((int)fd, check_buffer, sizeof(check_buffer), 4 * PAGE_SIZE),
              (s64)sizeof(check_buffer), "and read back");
    expect(memeq(page_buffer, check_buffer, sizeof(page_buffer)), "unchanged");

    for (u64 at = 0; at < 3000; at++) page_buffer[at] = (u8)(at * 7);
    expect_eq(lseek((int)fd, 4 * PAGE_SIZE + 1000, 0), (s64)(4 * PAGE_SIZE + 1000),
              "a byte offset inside a page is reached");
    expect_eq(write((int)fd, page_buffer, 3000), 3000, "an unaligned write lands");
    expect_eq(pread((int)fd, check_buffer, 3000, 4 * PAGE_SIZE + 1000), 3000,
              "and the unaligned range reads back");
    expect(memeq(page_buffer, check_buffer, 3000), "with the new bytes");

    for (u64 at = 0; at < PAGE_SIZE; at++) page_buffer[at] = 0xA5;
    u64 same_before = sysfs_number("mm_stat", 5);
    expect_eq(lseek((int)fd, 20 * PAGE_SIZE, 0), (s64)(20 * PAGE_SIZE), "seeking again works");
    expect_eq(write((int)fd, page_buffer, PAGE_SIZE), (s64)PAGE_SIZE,
              "a same-filled page is written");
    expect_eq((s64)sysfs_number("mm_stat", 5), (s64)(same_before + 1),
              "mm_stat counts it as a same-filled page");
    expect_eq(pread((int)fd, check_buffer, PAGE_SIZE, 20 * PAGE_SIZE), (s64)PAGE_SIZE,
              "it reads back");
    expect(memeq(page_buffer, check_buffer, PAGE_SIZE), "filled with the same byte");
    close((int)fd);
}

static int make_swap_header(void) {
    for (u64 at = 0; at < PAGE_SIZE; at++) page_buffer[at] = 0;
    u32 *info = (u32 *)(page_buffer + 1024);
    info[0] = 1;
    info[1] = (u32)(DISK_MIB * (1UL << 20) / PAGE_SIZE - 1);
    const char magic[] = "SWAPSPACE2";
    for (u64 at = 0; at < 10; at++) page_buffer[4086 + at] = (u8)magic[at];
    s64 fd = open(DEVICE, O_WRONLY, 0);
    if (fd < 0) return 0;
    int ok = write((int)fd, page_buffer, PAGE_SIZE) == (s64)PAGE_SIZE;
    close((int)fd);
    return ok;
}

static int swaps_line_has(const char *needle) {
    char text[1024];
    if (read_text("/proc/swaps", text, sizeof(text)) != 0) return 0;
    u64 wanted = strlen(needle);
    for (u64 at = 0; text[at]; at++)
        if (memeq(text + at, needle, wanted)) return 1;
    return 0;
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char text[128];
    expect(read_text(SYSFS "disksize", text, sizeof(text)) == 0 && streq(text, "0\n"),
           "zram0 starts without a size");
    expect(read_text(SYSFS "comp_algorithm", text, sizeof(text)) == 0 && streq(text, "[lz4]\n"),
           "and compresses with lz4");
    expect_eq(write_text(SYSFS "disksize", "512M\n"), 5, "disksize accepts 512M");
    expect_eq((s64)sysfs_number("disksize", 0), (s64)(DISK_MIB << 20), "disksize reads back");
    expect_eq((s64)sysfs_number("size", 0), (s64)(DISK_MIB << 11), "size is in sectors");
    expect_eq((s64)sysfs_number("initstate", 0), 1, "the device is initialised");
    expect_eq(write_text(SYSFS "disksize", "64M"), -EBUSY, "the size cannot change while set");
    block_io();

    expect(make_swap_header(), "a swap header is written to the device");
    expect_eq(swapon(DEVICE, PREFER | 100), 0, "swapon accepts zram0 with priority 100");
    expect(swaps_line_has(DEVICE), "/proc/swaps lists it");
    expect(swaps_line_has("partition"), "as a partition");
    expect(swaps_line_has("100"), "with priority 100");
    expect_eq(write_text(SYSFS "reset", "1"), -EBUSY, "reset is refused while it swaps");

    struct sysinfo info;
    sysinfo(&info);
    u64 bytes = info.totalram + (128UL << 20);
    u64 pages = bytes / PAGE_SIZE;
    u8 *area = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    expect((s64)area > 0, "more memory than RAM is mapped");
    fill(area, pages, 7);
    expect_eq((s64)mismatches(area, pages, 7), 0, "every page survives being compressed");
    u64 original = sysfs_number("mm_stat", 0);
    u64 compressed = sysfs_number("mm_stat", 1);
    expect(original > (64UL << 20), "zram holds more than 64 MiB of swapped data");
    expect(compressed * 10 < original, "compressed to less than a tenth");

    s64 child = fork();
    if (child == 0) {
        u64 wrong = mismatches(area, pages, 7);
        fill(area, pages / 4, 8);
        exit(wrong == 0 && mismatches(area, pages / 4, 8) == 0 ? 0 : 1);
    }
    int status = -1;
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "a child reads and rewrites zram-backed pages");
    expect_eq((s64)mismatches(area, pages, 7), 0, "the parent keeps its own copy");

    u64 keep = (32UL << 20) / PAGE_SIZE;
    u64 before = sysfs_number("mm_stat", 0);
    munmap(area + keep * PAGE_SIZE, (pages - keep) * PAGE_SIZE);
    u64 after = before;
    for (int wait = 0; wait < 50 && after >= before; wait++) {
        sleep_ms(100);
        after = sysfs_number("mm_stat", 0);
    }
    expect(after < before, "freed swap slots are discarded from zram");

    expect_eq(swapoff(DEVICE), 0, "swapoff takes the pages back");
    expect_eq((s64)mismatches(area, keep, 7), 0, "and they are intact");
    munmap(area, keep * PAGE_SIZE);
    expect_eq(write_text(SYSFS "reset", "1"), 1, "reset works once swap is off");
    expect_eq((s64)sysfs_number("disksize", 0), 0, "and clears the size");
    expect_eq((s64)sysfs_number("mm_stat", 0), 0, "and the stored data");
}
