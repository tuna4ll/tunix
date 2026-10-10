typedef long s64;
typedef unsigned long u64;

#if defined(__x86_64__)
#define SYS_write      1
#define SYS_getpid     39
#define SYS_reboot     169
#define SYS_exit_group 231
#elif defined(__aarch64__)
#define SYS_write      64
#define SYS_getpid     172
#define SYS_reboot     142
#define SYS_exit_group 94
#else
#error "ci-boot is written for x86_64 and aarch64"
#endif

static s64 syscall4(s64 number, s64 a, s64 b, s64 c, s64 d) {
#if defined(__x86_64__)
    register s64 r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "+a"(number)
                     : "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return number;
#else
    register s64 x8 __asm__("x8") = number;
    register s64 x0 __asm__("x0") = a;
    register s64 x1 __asm__("x1") = b;
    register s64 x2 __asm__("x2") = c;
    register s64 x3 __asm__("x3") = d;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
#endif
}

static void print(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    syscall4(SYS_write, 1, (s64)text, (s64)length, 0);
}

__attribute__((used, noreturn)) static void ci_boot(void) {
    if (syscall4(SYS_getpid, 0, 0, 0, 0) == 1) {
        print("ci-boot: PASS\n");
    } else {
        print("ci-boot: FAIL not pid 1\n");
    }
    syscall4(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
    syscall4(SYS_exit_group, 1, 0, 0, 0);
    for (;;) {}
}

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call ci_boot\n"
        "    hlt\n");
#else
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov x29, #0\n"
        "    bl ci_boot\n"
        "    b .\n");
#endif
