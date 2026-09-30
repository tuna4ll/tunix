typedef unsigned long u64;
typedef long s64;

#if defined(__x86_64__)
#define NR_READ 0
#define NR_WRITE 1
#define NR_CLOSE 3
#define NR_FSTAT 5
#define NR_PREAD 17
#define NR_PWRITE 18
#define NR_FTRUNCATE 77
#define NR_STATFS 137
#define NR_SYNC 162
#define NR_MOUNT 165
#define NR_UMOUNT2 166
#define NR_GETDENTS64 217
#define NR_EXIT_GROUP 231
#define NR_OPENAT 257
#define NR_MKDIRAT 258
#define NR_UNLINKAT 263
#define NR_RENAMEAT 264
#define NR_LINKAT 265
#define NR_SYMLINKAT 266
#define NR_READLINKAT 267

static inline s64 call6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
    s64 r;
    register s64 r10 __asm__("r10") = d;
    register s64 r8 __asm__("r8") = e;
    register s64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#elif defined(__aarch64__)
#define NR_READ 63
#define NR_WRITE 64
#define NR_CLOSE 57
#define NR_FSTAT 80
#define NR_PREAD 67
#define NR_PWRITE 68
#define NR_FTRUNCATE 46
#define NR_STATFS 43
#define NR_SYNC 81
#define NR_MOUNT 40
#define NR_UMOUNT2 39
#define NR_GETDENTS64 61
#define NR_EXIT_GROUP 94
#define NR_OPENAT 56
#define NR_MKDIRAT 34
#define NR_UNLINKAT 35
#define NR_RENAMEAT 38
#define NR_LINKAT 37
#define NR_SYMLINKAT 36
#define NR_READLINKAT 78

static inline s64 call6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
    register s64 x8 __asm__("x8") = n;
    register s64 x0 __asm__("x0") = a;
    register s64 x1 __asm__("x1") = b;
    register s64 x2 __asm__("x2") = c;
    register s64 x3 __asm__("x3") = d;
    register s64 x4 __asm__("x4") = e;
    register s64 x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}
#endif

#define call1(n, a) call6(n, (s64)(a), 0, 0, 0, 0, 0)
#define call2(n, a, b) call6(n, (s64)(a), (s64)(b), 0, 0, 0, 0)
#define call3(n, a, b, c) call6(n, (s64)(a), (s64)(b), (s64)(c), 0, 0, 0)
#define call4(n, a, b, c, d) call6(n, (s64)(a), (s64)(b), (s64)(c), (s64)(d), 0, 0)
#define call5(n, a, b, c, d, e) call6(n, (s64)(a), (s64)(b), (s64)(c), (s64)(d), (s64)(e), 0)

#define AT_FDCWD -100
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_DIRECTORY 0200000
#define EBUSY 16

struct volume {
    const char *label;
    const char *mountpoint;
    const char *type;
    u64 block_size;
    u64 far;
    char device[16];
};

static struct volume volumes[] = {
    {"vol1k", "/mnt/a", "ext2", 1024, 80ULL << 20, ""},
    {"vol2k", "/mnt/b", "ext3", 2048, 600ULL << 20, ""},
    {"vol4k", "/tmp/c", "ext2", 4096, 5ULL << 30, ""},
};
#define VOLUME_COUNT (sizeof(volumes) / sizeof(volumes[0]))

static int failures;
static char path[512];
static char buffer[131072];
static char check[131072];

static u64 length_of(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    return length;
}

static void print(const char *text) {
    call3(NR_WRITE, 1, text, length_of(text));
}

static void print_number(u64 value) {
    char digits[24];
    int at = 23;
    digits[at] = 0;
    do {
        digits[--at] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(digits + at);
}

static void report(const struct volume *volume, const char *name, int ok, s64 value) {
    print(ok ? "EXT2VOL ok " : "EXT2VOL FAIL ");
    if (volume) {
        print(volume->label);
        print(" ");
    }
    print(name);
    print(" ");
    if (value < 0) {
        print("-");
        value = -value;
    }
    print_number((u64)value);
    print("\n");
    if (!ok) failures++;
}

static int text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int bytes_equal(const char *a, const char *b, u64 count) {
    for (u64 index = 0; index < count; index++)
        if (a[index] != b[index]) return 0;
    return 1;
}

static const char *at(const struct volume *volume, const char *name) {
    u64 length = 0;
    for (const char *part = volume->mountpoint; *part;) path[length++] = *part++;
    path[length++] = '/';
    while (*name) path[length++] = *name++;
    path[length] = 0;
    return path;
}

static void pattern(char *out, u64 count, u64 seed) {
    for (u64 index = 0; index < count; index++)
        out[index] = (char)((index * 131 + seed * 7 + index / 4096) & 0xFF);
}

static s64 write_file(const char *name, const char *data, u64 count) {
    s64 fd = call4(NR_OPENAT, AT_FDCWD, name, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return fd;
    s64 wrote = call3(NR_WRITE, fd, data, count);
    call1(NR_CLOSE, fd);
    return wrote;
}

static s64 read_file(const char *name, char *out, u64 count) {
    s64 fd = call4(NR_OPENAT, AT_FDCWD, name, O_RDONLY, 0);
    if (fd < 0) return fd;
    s64 got = call3(NR_READ, fd, out, count);
    call1(NR_CLOSE, fd);
    return got;
}

static s64 file_size(s64 fd) {
    static u64 stat[32];
    if (call2(NR_FSTAT, fd, stat) != 0) return -1;
    return (s64)stat[6];
}

static s64 count_entries(const char *name) {
    static char entries[16384];
    s64 fd = call4(NR_OPENAT, AT_FDCWD, name, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return fd;
    s64 count = 0;
    for (;;) {
        s64 length = call3(NR_GETDENTS64, fd, entries, sizeof(entries));
        if (length <= 0) break;
        for (s64 offset = 0; offset < length;) {
            const char *entry = entries + offset + 19;
            if (!text_equal(entry, ".") && !text_equal(entry, "..")) count++;
            offset += *(unsigned short *)(entries + offset + 16);
        }
    }
    call1(NR_CLOSE, fd);
    return count;
}

static void find_devices(void) {
    call3(NR_MKDIRAT, AT_FDCWD, "/mnt", 0755);
    call3(NR_MKDIRAT, AT_FDCWD, "/mnt/probe", 0755);
    char device[16] = "/dev/sdb";
    for (char letter = 'b'; letter <= 'z'; letter++) {
        device[7] = letter;
        const char *types[] = {"ext2", "ext3"};
        for (int type = 0; type < 2; type++) {
            if (call5(NR_MOUNT, device, "/mnt/probe", types[type], 0, 0) != 0) continue;
            static char label[16];
            s64 got = read_file("/mnt/probe/label", label, sizeof(label) - 1);
            label[got > 0 ? got : 0] = 0;
            if (got > 0 && label[got - 1] == '\n') label[got - 1] = 0;
            for (u64 index = 0; index < VOLUME_COUNT; index++)
                if (text_equal(label, volumes[index].label))
                    for (int copy = 0; copy < 16; copy++)
                        volumes[index].device[copy] = device[copy];
            call2(NR_UMOUNT2, "/mnt/probe", 0);
            break;
        }
    }
}

static int mount_volume(struct volume *volume) {
    call3(NR_MKDIRAT, AT_FDCWD, volume->mountpoint, 0755);
    s64 status = call5(NR_MOUNT, volume->device, volume->mountpoint, volume->type, 0, 0);
    report(volume, "mount", status == 0, status);
    return status == 0;
}

static void exercise(struct volume *volume) {
    static char seed[64];
    s64 got = read_file(at(volume, "seed.txt"), seed, sizeof(seed));
    report(volume, "seed-read", got == 11 && bytes_equal(seed, "seed-", 5), got);

    s64 entries = count_entries(at(volume, "big"));
    report(volume, "indexed-directory-entries", entries == 400, entries);
    s64 status = write_file(at(volume, "big/added-after-index"), "x", 1);
    report(volume, "indexed-directory-add", status == 1, status);
    status = call3(NR_UNLINKAT, AT_FDCWD, at(volume, "big/f17"), 0);
    report(volume, "indexed-directory-remove", status == 0, status);

    status = call3(NR_UNLINKAT, AT_FDCWD, at(volume, "attr.txt"), 0);
    report(volume, "xattr-file-unlink", status == 0, status);

    call3(NR_MKDIRAT, AT_FDCWD, at(volume, "d1"), 0755);
    status = call3(NR_MKDIRAT, AT_FDCWD, at(volume, "d1/d2"), 0755);
    report(volume, "mkdir", status == 0, status);
    pattern(buffer, 100000, volume->block_size);
    status = write_file(at(volume, "d1/d2/data"), buffer, 100000);
    got = read_file(at(volume, "d1/d2/data"), check, sizeof(check));
    report(volume, "data-round-trip", status == 100000 && got == 100000 &&
           bytes_equal(buffer, check, 100000), got);

    s64 fd = call4(NR_OPENAT, AT_FDCWD, at(volume, "far"), O_RDWR | O_CREAT, 0644);
    status = call4(NR_PWRITE, fd, "farfar!!", 8, volume->far);
    static char tail[8];
    got = call4(NR_PREAD, fd, tail, 8, volume->far);
    report(volume, "sparse-far-write", status == 8 && got == 8 && bytes_equal(tail, "farfar!!", 8) &&
           file_size(fd) == (s64)(volume->far + 8), file_size(fd));
    call1(NR_CLOSE, fd);

    static char target[200];
    for (int index = 0; index < 150; index++) target[index] = (char)('a' + index % 26);
    target[150] = 0;
    status = call3(NR_SYMLINKAT, target, AT_FDCWD, at(volume, "long-link"));
    static char link_back[256];
    got = call4(NR_READLINKAT, AT_FDCWD, at(volume, "long-link"), link_back, sizeof(link_back));
    report(volume, "slow-symlink", status == 0 && got == 150 && bytes_equal(link_back, target, 150), got);

    static char hard[128];
    const char *source = at(volume, "d1/d2/data");
    for (int index = 0; index < 128; index++) hard[index] = source[index];
    status = call5(NR_LINKAT, AT_FDCWD, hard, AT_FDCWD, at(volume, "d1/hard"), 0);
    report(volume, "hard-link", status == 0, status);

    fd = call4(NR_OPENAT, AT_FDCWD, at(volume, "ghost"), O_RDWR | O_CREAT, 0644);
    pattern(buffer, 20000, 99);
    call3(NR_WRITE, fd, buffer, 20000);
    call1(NR_SYNC, 0);
    status = call3(NR_UNLINKAT, AT_FDCWD, at(volume, "ghost"), 0);
    got = call4(NR_PREAD, fd, check, 20000, 0);
    report(volume, "open-unlinked-read", status == 0 && got == 20000 &&
           bytes_equal(buffer, check, 20000), got);
    call1(NR_CLOSE, fd);

    fd = call4(NR_OPENAT, AT_FDCWD, at(volume, "d1/hard"), O_RDWR, 0);
    status = call2(NR_FTRUNCATE, fd, 5000);
    report(volume, "truncate", status == 0 && file_size(fd) == 5000, file_size(fd));
    call1(NR_CLOSE, fd);

    static u64 statfs[16];
    status = call2(NR_STATFS, at(volume, "d1"), statfs);
    report(volume, "statfs-block-size", status == 0 && statfs[1] == volume->block_size, (s64)statfs[1]);

    fd = call4(NR_OPENAT, AT_FDCWD, at(volume, "seed.txt"), O_RDONLY, 0);
    status = call2(NR_UMOUNT2, volume->mountpoint, 0);
    report(volume, "umount-busy", status == -EBUSY, status);
    call1(NR_CLOSE, fd);
}

static void verify_after_remount(struct volume *volume) {
    s64 got = read_file(at(volume, "d1/hard"), check, sizeof(check));
    pattern(buffer, 5000, volume->block_size);
    report(volume, "remount-truncated-data", got == 5000 && bytes_equal(buffer, check, 5000), got);
    s64 fd = call4(NR_OPENAT, AT_FDCWD, at(volume, "far"), O_RDONLY, 0);
    static char tail[8];
    got = call4(NR_PREAD, fd, tail, 8, volume->far);
    report(volume, "remount-sparse-far", got == 8 && bytes_equal(tail, "farfar!!", 8) &&
           file_size(fd) == (s64)(volume->far + 8), file_size(fd));
    call1(NR_CLOSE, fd);
    s64 entries = count_entries(at(volume, "big"));
    report(volume, "remount-directory-entries", entries == 400, entries);
    got = read_file(at(volume, "ghost"), check, 16);
    report(volume, "remount-ghost-gone", got < 0, got);
}

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    find_devices();
    int mounted = 1;
    for (u64 index = 0; index < VOLUME_COUNT; index++) {
        report(&volumes[index], "found", volumes[index].device[0] != 0, 0);
        if (!volumes[index].device[0] || !mount_volume(&volumes[index])) mounted = 0;
    }
    if (mounted) {
        s64 status = call5(NR_MOUNT, volumes[0].device, "/mnt/probe", "ext2", 0, 0);
        report(0, "mount-twice-busy", status == -EBUSY, status);
        for (u64 index = 0; index < VOLUME_COUNT; index++) exercise(&volumes[index]);

        pattern(buffer, 70000, 5);
        write_file("/mnt/a/travel", buffer, 70000);
        s64 status_move = call4(NR_RENAMEAT, AT_FDCWD, "/mnt/a/travel", AT_FDCWD, "/mnt/b/moved");
        s64 got = read_file("/mnt/b/moved", check, sizeof(check));
        report(0, "cross-volume-rename", status_move == 0 && got == 70000 &&
               bytes_equal(buffer, check, 70000), got);
        got = read_file("/mnt/a/travel", check, 16);
        report(0, "cross-volume-source-gone", got < 0, got);

        call1(NR_SYNC, 0);
        for (u64 index = 0; index < VOLUME_COUNT; index++) {
            s64 status_umount = call2(NR_UMOUNT2, volumes[index].mountpoint, 0);
            report(&volumes[index], "umount", status_umount == 0, status_umount);
        }
        for (u64 index = 0; index < VOLUME_COUNT; index++) {
            if (!mount_volume(&volumes[index])) continue;
            verify_after_remount(&volumes[index]);
            s64 status_umount = call2(NR_UMOUNT2, volumes[index].mountpoint, 0);
            report(&volumes[index], "final-umount", status_umount == 0, status_umount);
        }
    }
    print(failures ? "EXT2TEST FAIL\n" : "EXT2TEST PASS\n");
    call1(NR_EXIT_GROUP, 0);
    for (;;) { }
}

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call run\n"
        "    hlt\n");
#else
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov x29, #0\n"
        "    bl run\n"
        "    b .\n");
#endif
