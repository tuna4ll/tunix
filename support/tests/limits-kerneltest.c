typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;

#if defined(__x86_64__)
#define NR_READ 0
#define NR_WRITE 1
#define NR_CLOSE 3
#define NR_MMAP 9
#define NR_MUNMAP 11
#define NR_EXIT_GROUP 231
#define NR_OPENAT 257
#define NR_GETCWD 79
#define NR_CHDIR 80
#define NR_GETDENTS64 217
#define NR_MKDIRAT 258
#define NR_NEWFSTATAT 262
#define NR_UNLINKAT 263
#define NR_RENAMEAT 264
#define NR_SYMLINKAT 266
#define NR_READLINKAT 267
#define NR_FORK 57
#define NR_EXIT 60
#define NR_WAIT4 61
#define NR_PIPE2 293
#define NR_DUP 32
#define NR_DUP2_OR_3 33
#define NR_FCNTL 72
#define NR_POLL_OR_PPOLL 7
#define NR_PSELECT6 270
#define NR_PRLIMIT64 302
#define NR_CLOSE_RANGE 436
#define NR_SETUID 105
#define NR_CLOCK_GETTIME 228
#define NR_EXECVE 59


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
#define NR_MMAP 222
#define NR_MUNMAP 215
#define NR_EXIT_GROUP 94
#define NR_OPENAT 56
#define NR_GETCWD 17
#define NR_CHDIR 49
#define NR_GETDENTS64 61
#define NR_MKDIRAT 34
#define NR_NEWFSTATAT 79
#define NR_UNLINKAT 35
#define NR_RENAMEAT 38
#define NR_SYMLINKAT 36
#define NR_READLINKAT 78
#define NR_CLONE 220
#define NR_EXIT 93
#define NR_WAIT4 260
#define NR_PIPE2 59
#define NR_DUP 23
#define NR_DUP2_OR_3 24
#define NR_FCNTL 25
#define NR_POLL_OR_PPOLL 73
#define NR_PSELECT6 72
#define NR_PRLIMIT64 261
#define NR_CLOSE_RANGE 436
#define NR_SETUID 146
#define NR_CLOCK_GETTIME 113
#define NR_EXECVE 221


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

#if defined(__x86_64__)
static inline s64 do_fork(void) { return call6(NR_FORK, 0, 0, 0, 0, 0, 0); }
#else
static inline s64 do_fork(void) { return call6(NR_CLONE, 17, 0, 0, 0, 0, 0); }
#endif

#define call0(n) call6(n, 0, 0, 0, 0, 0, 0)
#define call1(n, a) call6(n, (s64)(a), 0, 0, 0, 0, 0)
#define call2(n, a, b) call6(n, (s64)(a), (s64)(b), 0, 0, 0, 0)
#define call3(n, a, b, c) call6(n, (s64)(a), (s64)(b), (s64)(c), 0, 0, 0)
#define call4(n, a, b, c, d) call6(n, (s64)(a), (s64)(b), (s64)(c), (s64)(d), 0, 0)

#define AT_FDCWD -100
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0100
#define O_DIRECTORY 0200000
#define AT_REMOVEDIR 0x200
#define ENOENT 2
#define ENAMETOOLONG 36
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

static int failures;

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

static void report(const char *name, int ok) {
    print(ok ? "LIMITS ok " : "LIMITS FAIL ");
    print(name);
    print("\n");
    if (!ok) failures++;
}

static void report_value(const char *name, int ok, u64 value) {
    print(ok ? "LIMITS ok " : "LIMITS FAIL ");
    print(name);
    print(" ");
    print_number(value);
    print("\n");
    if (!ok) failures++;
}

static int starts_with(const char *text, const char *prefix) {
    while (*prefix)
        if (*text++ != *prefix++) return 0;
    return 1;
}

static u64 meminfo_kib(const char *key) {
    static char buffer[4096];
    s64 fd = call4(NR_OPENAT, AT_FDCWD, "/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) return 0;
    s64 length = call3(NR_READ, fd, buffer, sizeof(buffer) - 1);
    call1(NR_CLOSE, fd);
    if (length <= 0) return 0;
    buffer[length] = 0;
    for (char *line = buffer; *line;) {
        if (starts_with(line, key)) {
            char *at = line + length_of(key);
            while (*at == ' ' || *at == ':') at++;
            u64 value = 0;
            while (*at >= '0' && *at <= '9') value = value * 10 + (u64)(*at++ - '0');
            return value;
        }
        while (*line && *line != '\n') line++;
        if (*line) line++;
    }
    return 0;
}

static void test_memory(void) {
    u64 total = meminfo_kib("MemTotal");
    if (total < 9ULL * 1024 * 1024) {
        report_value("memory-small-machine-kib", total > 0, total);
        return;
    }
    report_value("memory-total-above-8g-kib", 1, total);
    u64 bytes = total * 1024ULL - 1536ULL * 1024 * 1024;
    u64 chunk = 256ULL * 1024 * 1024;
    u64 count = bytes / chunk;
    static u64 chunks[128];
    if (count > 128) count = 128;
    u64 mapped = 0;
    for (; mapped < count; mapped++) {
        s64 address = call6(NR_MMAP, 0, (s64)chunk, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (address < 0 && address > -4096) break;
        chunks[mapped] = (u64)address;
        for (u64 page = 0; page < chunk; page += 4096)
            *(volatile u64 *)(chunks[mapped] + page) = chunks[mapped] + page;
    }
    int intact = mapped == count;
    for (u64 index = 0; index < mapped && intact; index++)
        for (u64 page = 0; page < chunk; page += 4096)
            if (*(volatile u64 *)(chunks[index] + page) != chunks[index] + page) {
                intact = 0;
                break;
            }
    report_value("memory-touched-mib", intact && mapped * chunk > 8ULL * 1024 * 1024 * 1024,
                 mapped * chunk / (1024 * 1024));
    for (u64 index = 0; index < mapped; index++) call2(NR_MUNMAP, chunks[index], chunk);
    u64 free_after = meminfo_kib("MemFree");
    report_value("memory-returned-kib", free_after + 1024 * 1024 > bytes / 1024, free_after);
}

static char big_path[8192];
static char stat_buffer[256];

static void fill(char *out, char letter, u64 count) {
    for (u64 index = 0; index < count; index++) out[index] = letter;
    out[count] = 0;
}

static u64 append(char *out, u64 at, const char *text) {
    while (*text) out[at++] = *text++;
    out[at] = 0;
    return at;
}

static int text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int directory_lists(const char *path, const char *wanted) {
    static char entries[16384];
    s64 fd = call4(NR_OPENAT, AT_FDCWD, path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return 0;
    int found = 0;
    for (;;) {
        s64 length = call3(NR_GETDENTS64, fd, entries, sizeof(entries));
        if (length <= 0) break;
        for (s64 at = 0; at < length;) {
            unsigned short record = *(unsigned short *)(entries + at + 16);
            if (text_equal(entries + at + 19, wanted)) found = 1;
            at += record;
        }
    }
    call1(NR_CLOSE, fd);
    return found;
}

static void test_long_names(void) {
    char name[300];
    char other[300];
    fill(name, 'n', 255);
    fill(other, 'm', 255);
    u64 at = append(big_path, 0, "/tmp/");
    at = append(big_path, at, name);
    s64 made = call3(NR_MKDIRAT, AT_FDCWD, big_path, 0755);
    report("name-255-mkdir", made == 0);
    report("name-255-readdir", directory_lists("/tmp", name));
    u64 file_at = append(big_path, at, "/");
    file_at = append(big_path, file_at, other);
    s64 fd = call4(NR_OPENAT, AT_FDCWD, big_path, O_WRONLY | O_CREAT, 0644);
    report("name-255-create", fd >= 0);
    if (fd >= 0) call1(NR_CLOSE, fd);
    report("name-255-stat", call4(NR_NEWFSTATAT, AT_FDCWD, big_path, stat_buffer, 0) == 0);
    static char renamed[8192];
    u64 renamed_at = append(renamed, 0, "/tmp/");
    renamed_at = append(renamed, renamed_at, name);
    renamed_at = append(renamed, renamed_at, "/");
    fill(other, 'r', 255);
    append(renamed, renamed_at, other);
    report("name-255-rename", call4(NR_RENAMEAT, AT_FDCWD, big_path, AT_FDCWD, renamed) == 0);
    report("name-255-unlink", call3(NR_UNLINKAT, AT_FDCWD, renamed, 0) == 0);
    big_path[at] = 0;
    report("name-255-rmdir-done", call3(NR_UNLINKAT, AT_FDCWD, big_path, AT_REMOVEDIR) == 0);
    fill(name, 'x', 256);
    at = append(big_path, 0, "/tmp/");
    append(big_path, at, name);
    report_value("name-256-rejected", call3(NR_MKDIRAT, AT_FDCWD, big_path, 0755) == -ENAMETOOLONG,
                 (u64)-call3(NR_MKDIRAT, AT_FDCWD, big_path, 0755));
}

static const char *big_path_file(u64 at) {
    append(big_path, at, "/f");
    return big_path;
}

static void test_long_paths(void) {
    char name[256];
    fill(name, 'p', 250);
    u64 at = append(big_path, 0, "/tmp");
    int levels = 0;
    int made = 1;
    while (at + 252 < 4000) {
        at = append(big_path, at, "/");
        at = append(big_path, at, name);
        if (call3(NR_MKDIRAT, AT_FDCWD, big_path, 0755) != 0) {
            made = 0;
            break;
        }
        levels++;
    }
    report_value("path-levels-made", made, (u64)levels);
    report_value("path-length", made, at);
    s64 fd = call4(NR_OPENAT, AT_FDCWD, big_path_file(at), O_WRONLY | O_CREAT, 0644);
    report("path-4k-create", fd >= 0);
    if (fd >= 0) {
        report("path-4k-write", call3(NR_WRITE, fd, "hello", 5) == 5);
        call1(NR_CLOSE, fd);
    }
    big_path[at] = 0;
    report("path-4k-chdir", call1(NR_CHDIR, big_path) == 0);
    static char cwd[8192];
    s64 length = call2(NR_GETCWD, cwd, sizeof(cwd));
    report_value("path-4k-getcwd", length == (s64)at + 1 && text_equal(cwd, big_path), (u64)length);
    s64 link_length = call4(NR_READLINKAT, AT_FDCWD, "/proc/self/cwd", cwd, sizeof(cwd));
    report_value("path-4k-proc-cwd", link_length == (s64)at, (u64)link_length);
    fd = call4(NR_OPENAT, AT_FDCWD, "f", O_RDONLY, 0);
    report("path-4k-relative-open", fd >= 0);
    if (fd >= 0) call1(NR_CLOSE, fd);
    call1(NR_CHDIR, "/");
    report("path-4k-unlink", call3(NR_UNLINKAT, AT_FDCWD, big_path_file(at), 0) == 0);
    big_path[at] = 0;
    u64 over = append(big_path, at, "/");
    fill(name, 'q', 255);
    over = append(big_path, over, name);
    over = append(big_path, over, "/");
    fill(name, 'z', 200);
    over = append(big_path, over, name);
    report_value("path-over-4k-rejected",
                 call4(NR_OPENAT, AT_FDCWD, big_path, O_RDONLY, 0) == -ENAMETOOLONG, over);
    big_path[at] = 0;
    int removed = 1;
    for (u64 end = at; end > 4;) {
        big_path[end] = 0;
        if (call3(NR_UNLINKAT, AT_FDCWD, big_path, AT_REMOVEDIR) != 0) {
            removed = 0;
            break;
        }
        while (end > 0 && big_path[end - 1] != '/') end--;
        end--;
    }
    report("path-4k-cleanup", removed);
}

static void test_deep_tree(void) {
    report("deep-start", call3(NR_MKDIRAT, AT_FDCWD, "/tmp/deep", 0755) == 0 &&
                         call1(NR_CHDIR, "/tmp/deep") == 0);
    int depth = 0;
    for (; depth < 300; depth++)
        if (call3(NR_MKDIRAT, AT_FDCWD, "d", 0755) != 0 || call1(NR_CHDIR, "d") != 0) break;
    report_value("deep-depth", depth == 300, (u64)depth);
    static char cwd[8192];
    s64 length = call2(NR_GETCWD, cwd, sizeof(cwd));
    report_value("deep-getcwd", length == 10 + 2 * 300, (u64)length);
    s64 fd = call4(NR_OPENAT, AT_FDCWD, "leaf", O_WRONLY | O_CREAT, 0644);
    report("deep-create", fd >= 0);
    if (fd >= 0) call1(NR_CLOSE, fd);
    report("deep-absolute-stat", call4(NR_NEWFSTATAT, AT_FDCWD, cwd, stat_buffer, 0) == 0);
    call3(NR_UNLINKAT, AT_FDCWD, "leaf", 0);
    int removed = 1;
    for (int level = 0; level < depth; level++) {
        call1(NR_CHDIR, "..");
        if (call3(NR_UNLINKAT, AT_FDCWD, "d", AT_REMOVEDIR) != 0) removed = 0;
    }
    call1(NR_CHDIR, "/");
    report("deep-cleanup", removed && call3(NR_UNLINKAT, AT_FDCWD, "/tmp/deep", AT_REMOVEDIR) == 0);
}

static void test_symlink_chain(void) {
    report("chain-target", call3(NR_MKDIRAT, AT_FDCWD, "/tmp/chain", 0755) == 0);
    s64 fd = call4(NR_OPENAT, AT_FDCWD, "/tmp/chain/l0", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) call1(NR_CLOSE, fd);
    char link[32];
    char target[32];
    int made = 1;
    for (int index = 1; index <= 45; index++) {
        u64 at = append(link, 0, "/tmp/chain/l");
        at = append(target, 0, "l");
        char digits[4] = {(char)('0' + index / 10), (char)('0' + index % 10), 0, 0};
        char previous[4] = {(char)('0' + (index - 1) / 10), (char)('0' + (index - 1) % 10), 0, 0};
        append(link, 12, index < 10 ? digits + 1 : digits);
        append(target, 1, index - 1 < 10 ? previous + 1 : previous);
        (void)at;
        if (call3(NR_SYMLINKAT, target, AT_FDCWD, link) != 0) made = 0;
    }
    report("chain-made", made);
    fd = call4(NR_OPENAT, AT_FDCWD, "/tmp/chain/l30", O_RDONLY, 0);
    report("chain-30-resolves", fd >= 0);
    if (fd >= 0) call1(NR_CLOSE, fd);
    fd = call4(NR_OPENAT, AT_FDCWD, "/tmp/chain/l45", O_RDONLY, 0);
    report("chain-45-refused", fd < 0);
    if (fd >= 0) call1(NR_CLOSE, fd);
    static char long_target[4200];
    fill(long_target, 'a', 3000);
    long_target[0] = '/';
    report("symlink-long-target", call3(NR_SYMLINKAT, long_target, AT_FDCWD, "/tmp/chain/long") == 0);
    static char back[4200];
    s64 got = call4(NR_READLINKAT, AT_FDCWD, "/tmp/chain/long", back, sizeof(back));
    report_value("symlink-long-readlink", got == 3000, (u64)got);
    int removed = 1;
    for (int index = 0; index <= 45; index++) {
        u64 at = append(link, 0, "/tmp/chain/l");
        char digits[4] = {(char)('0' + index / 10), (char)('0' + index % 10), 0, 0};
        append(link, at, index < 10 ? digits + 1 : digits);
        if (call3(NR_UNLINKAT, AT_FDCWD, link, 0) != 0) removed = 0;
    }
    call3(NR_UNLINKAT, AT_FDCWD, "/tmp/chain/long", 0);
    report("chain-cleanup", removed && call3(NR_UNLINKAT, AT_FDCWD, "/tmp/chain", AT_REMOVEDIR) == 0);
}

static u64 now_ms(void) {
    u64 time[2];
    call2(NR_CLOCK_GETTIME, 1, time);
    return time[0] * 1000 + time[1] / 1000000;
}

static void test_many_processes(u64 wanted) {
    u64 started_ms = now_ms();
    int pipe_fds[2];
    if (call2(NR_PIPE2, pipe_fds, 0) != 0) {
        report("processes-pipe", 0);
        return;
    }
    u64 started = 0;
    for (; started < wanted; started++) {
        s64 pid = do_fork();
        if (pid == 0) {
            char byte;
            call1(NR_CLOSE, pipe_fds[1]);
            call3(NR_READ, pipe_fds[0], &byte, 1);
            call1(NR_EXIT, 7);
        }
        if (pid < 0) break;
    }
    report_value("processes-alive", started == wanted, started);
    u64 forked_ms = now_ms();
    call1(NR_CLOSE, pipe_fds[1]);
    u64 reaped = 0;
    int all_seven = 1;
    for (;;) {
        int status = 0;
        s64 pid = call4(NR_WAIT4, -1, &status, 0, 0);
        if (pid <= 0) break;
        if (((status >> 8) & 0xFF) != 7) all_seven = 0;
        reaped++;
    }
    call1(NR_CLOSE, pipe_fds[0]);
    report_value("processes-reaped", reaped == started && all_seven, reaped);
    report_value("processes-fork-ms", 1, forked_ms - started_ms);
    report_value("processes-reap-ms", 1, now_ms() - forked_ms);
}

struct rlimit_pair {
    u64 soft;
    u64 hard;
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

static s64 dup_to(int fd, int target) {
#if defined(__x86_64__)
    return call2(NR_DUP2_OR_3, fd, target);
#else
    return call3(NR_DUP2_OR_3, fd, target, 0);
#endif
}

static s64 poll_now(struct pollfd *fds, u64 count) {
#if defined(__x86_64__)
    return call3(NR_POLL_OR_PPOLL, fds, count, 0);
#else
    u64 zero[2] = {0, 0};
    return call4(NR_POLL_OR_PPOLL, fds, count, zero, 0);
#endif
}

static void test_descriptors(void) {
    struct rlimit_pair limit;
    report("nofile-get", call4(NR_PRLIMIT64, 0, 7, 0, &limit) == 0);
    report_value("nofile-default-soft", limit.soft == 1024, limit.soft);
    report_value("nofile-default-hard", limit.hard == 1048576, limit.hard);
    struct rlimit_pair raised = {6000, limit.hard};
    report("nofile-raise", call4(NR_PRLIMIT64, 0, 7, &raised, 0) == 0);
    int pipe_fds[2];
    call2(NR_PIPE2, pipe_fds, 0);
    call3(NR_WRITE, pipe_fds[1], "x", 1);
    s64 highest = -1;
    u64 opened = 0;
    for (u64 index = 0; index < 5000; index++) {
        s64 fd = call1(NR_DUP, pipe_fds[0]);
        if (fd < 0) break;
        highest = fd;
        opened++;
    }
    report_value("fds-opened", opened == 5000, opened);
    report_value("fds-highest", highest >= 5000, (u64)highest);
    report("fds-dup2-inside", dup_to(pipe_fds[0], 5999) == 5999);
    report("fds-dup2-outside", dup_to(pipe_fds[0], 6000) == -9);
    static struct pollfd polled[4000];
    for (int index = 0; index < 4000; index++) {
        polled[index].fd = 1000 + index;
        polled[index].events = 1;
        polled[index].revents = 0;
    }
    s64 ready = poll_now(polled, 4000);
    report_value("fds-poll-4000", ready == 4000 && polled[3999].revents == 1, (u64)ready);
    static u64 read_set[6000 / 64 + 1];
    for (u64 word = 0; word < sizeof(read_set) / 8; word++) read_set[word] = 0;
    read_set[5999 / 64] |= 1ULL << (5999 % 64);
    u64 zero_time[2] = {0, 0};
    s64 selected = call6(NR_PSELECT6, 6000, (s64)read_set, 0, 0, (s64)zero_time, 0);
    report_value("fds-select-5999", selected == 1 && (read_set[5999 / 64] >> (5999 % 64)) & 1,
                 (u64)selected);
    s64 pid = do_fork();
    if (pid == 0) {
        int ok = call3(NR_FCNTL, 5999, 1, 0) >= 0 && call3(NR_FCNTL, 4999, 1, 0) >= 0;
        call1(NR_EXIT, ok ? 0 : 1);
    }
    int status = -1;
    call4(NR_WAIT4, pid, &status, 0, 0);
    report_value("fds-inherited-by-fork", status == 0, (u64)status);
    report("fds-close-range", call3(NR_CLOSE_RANGE, 3, ~0U, 0) == 0 &&
                              call3(NR_FCNTL, 5999, 1, 0) == -9 && call3(NR_FCNTL, 3, 1, 0) == -9);
    struct rlimit_pair lowered = {10, limit.hard};
    call4(NR_PRLIMIT64, 0, 7, &lowered, 0);
    u64 under_low = 0;
    for (;;) {
        s64 fd = call1(NR_DUP, 0);
        if (fd < 0) {
            report_value("fds-emfile-at-soft-limit", fd == -24 && under_low == 7, under_low);
            break;
        }
        under_low++;
        if (under_low > 20) {
            report("fds-emfile-at-soft-limit", 0);
            break;
        }
    }
    call3(NR_CLOSE_RANGE, 3, ~0U, 0);
    pid = do_fork();
    if (pid == 0) {
        call1(NR_SETUID, 1000);
        struct rlimit_pair above = {10, 2000000};
        struct rlimit_pair within = {10, 20};
        int ok = call4(NR_PRLIMIT64, 0, 7, &above, 0) == -1 &&
                 call4(NR_PRLIMIT64, 0, 7, &within, 0) == 0;
        call1(NR_EXIT, ok ? 0 : 1);
    }
    call4(NR_WAIT4, pid, &status, 0, 0);
    report_value("nofile-unprivileged-hard", status == 0, (u64)status);
    call4(NR_PRLIMIT64, 0, 7, &limit, 0);
}

static char big_argument_area[7 * 1024 * 1024];
static const char *exec_vector[70000];

static u64 fill_arguments(u64 count, u64 each, char letter) {
    char *at = big_argument_area;
    exec_vector[0] = "/sbin/init";
    exec_vector[1] = "argcheck";
    for (u64 index = 0; index < count; index++) {
        exec_vector[index + 2] = at;
        for (u64 byte = 0; byte < each; byte++) at[byte] = letter;
        at[each] = 0;
        at += each + 1;
    }
    exec_vector[count + 2] = 0;
    return count + 2;
}

static int exec_child(u64 stack_limit) {
    s64 pid = do_fork();
    if (pid == 0) {
        if (stack_limit) {
            struct rlimit_pair value = {stack_limit, ~0UL};
            call4(NR_PRLIMIT64, 0, 3, &value, 0);
        }
        static const char *environment[] = {"LIMITS=1", 0};
        s64 result = call3(NR_EXECVE, "/sbin/init", exec_vector, environment);
        call1(NR_EXIT, result == -7 ? 77 : 99);
    }
    int status = -1;
    call4(NR_WAIT4, pid, &status, 0, 0);
    return (status >> 8) & 0xFF;
}

static int argument_check(u64 *stack) {
    u64 argc = stack[0];
    const char **argv = (const char **)(stack + 1);
    u64 total = 0;
    for (u64 index = 0; index < argc; index++) total += length_of(argv[index]) + 1;
    static char cmdline[8 * 1024 * 1024];
    s64 fd = call4(NR_OPENAT, AT_FDCWD, "/proc/self/cmdline", O_RDONLY, 0);
    u64 got = 0;
    for (;;) {
        s64 amount = call3(NR_READ, fd, cmdline + got, sizeof(cmdline) - got);
        if (amount <= 0) break;
        got += (u64)amount;
    }
    call1(NR_CLOSE, fd);
    if (got != total) return 3;
    const char *last = argv[argc - 1];
    if (argc > 2 && last[0] != argv[2][0]) return 4;
    return 0;
}

static int deep(int levels) {
    volatile char frame[4096];
    frame[0] = (char)levels;
    frame[4095] = (char)levels;
    if (levels == 0) return frame[0];
    return deep(levels - 1) + frame[4095] - frame[4095];
}

static void test_exec_arguments(void) {
    fill_arguments(5000, 100, 'a');
    report_value("exec-5000-arguments", exec_child(0) == 0, 5000);
    fill_arguments(1, 100 * 1024, 'b');
    report_value("exec-100k-argument", exec_child(0) == 0, 100 * 1024);
    fill_arguments(1, 200 * 1024, 'c');
    report_value("exec-200k-argument-refused", exec_child(0) == 77, 200 * 1024);
    fill_arguments(60000, 40, 'd');
    report_value("exec-2.4m-refused-at-8m-stack", exec_child(0) == 77, 60000);
    report_value("exec-2.4m-with-64m-stack", exec_child(64ULL * 1024 * 1024) == 0, 60000);
    s64 pid = do_fork();
    if (pid == 0) {
        struct rlimit_pair value = {64ULL * 1024 * 1024, ~0UL};
        call4(NR_PRLIMIT64, 0, 3, &value, 0);
        call1(NR_EXIT, deep(8000) == 0 ? 0 : 1);
    }
    int status = -1;
    call4(NR_WAIT4, pid, &status, 0, 0);
    report_value("stack-32m-deep", status == 0, (u64)status);
}

#ifndef LIMITS_TESTS
#define LIMITS_TESTS 0xFFFFFFFFU
#endif
#ifndef LIMITS_PROCESSES
#define LIMITS_PROCESSES 2000
#endif

static void run(u64 *stack) __attribute__((noreturn, used));
static void run(u64 *stack) {
    if (stack[0] >= 2 && text_equal(((const char **)(stack + 1))[1], "argcheck"))
        call1(NR_EXIT_GROUP, argument_check(stack));
    if (LIMITS_TESTS & 0x01U) test_memory();
    if (LIMITS_TESTS & 0x02U) test_long_names();
    if (LIMITS_TESTS & 0x04U) test_long_paths();
    if (LIMITS_TESTS & 0x08U) test_deep_tree();
    if (LIMITS_TESTS & 0x10U) test_symlink_chain();
    if (LIMITS_TESTS & 0x20U) test_many_processes(LIMITS_PROCESSES);
    if (LIMITS_TESTS & 0x40U) test_descriptors();
    if (LIMITS_TESTS & 0x80U) test_exec_arguments();
    print(failures ? "LIMITSTEST FAIL\n" : "LIMITSTEST PASS\n");
    call1(NR_EXIT_GROUP, 0);
    for (;;) { }
}

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call run\n"
        "    hlt\n");
#else
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov x29, #0\n"
        "    mov x0, sp\n"
        "    bl run\n"
        "    b .\n");
#endif
