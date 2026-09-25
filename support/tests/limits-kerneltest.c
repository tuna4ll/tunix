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

#define call0(n) call6(n, 0, 0, 0, 0, 0, 0)
#define call1(n, a) call6(n, (s64)(a), 0, 0, 0, 0, 0)
#define call2(n, a, b) call6(n, (s64)(a), (s64)(b), 0, 0, 0, 0)
#define call3(n, a, b, c) call6(n, (s64)(a), (s64)(b), (s64)(c), 0, 0, 0)
#define call4(n, a, b, c, d) call6(n, (s64)(a), (s64)(b), (s64)(c), (s64)(d), 0, 0)

#define AT_FDCWD -100
#define O_RDONLY 0
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

static __attribute__((unused)) void report(const char *name, int ok) {
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

static void run(void) __attribute__((noreturn, used));
static void run(void) {
    test_memory();
    print(failures ? "LIMITSTEST FAIL\n" : "LIMITSTEST PASS\n");
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
