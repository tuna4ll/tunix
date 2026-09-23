#ifndef TUNIX_TEST_SYSCALL_H
#define TUNIX_TEST_SYSCALL_H

extern s64 spawn_thread(u64 flags, void *child_stack_top);

#if defined(__x86_64__)

static inline s64 syscall0(s64 n) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static inline s64 syscall1(s64 n, s64 a) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}

static inline s64 syscall2(s64 n, s64 a, s64 b) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline s64 syscall3(s64 n, s64 a, s64 b, s64 c) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static inline s64 syscall4(s64 n, s64 a, s64 b, s64 c, s64 d) {
    s64 r;
    register s64 r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static inline s64 syscall6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
    s64 r;
    register s64 r10 __asm__("r10") = d;
    register s64 r8 __asm__("r8") = e;
    register s64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

__asm__(".text\n"
        ".globl spawn_thread\n"
        "spawn_thread:\n"
        "    xor %edx, %edx\n"
        "    xor %r10d, %r10d\n"
        "    xor %r8d, %r8d\n"
        "    mov $56, %eax\n"
        "    syscall\n"
        "    test %rax, %rax\n"
        "    jnz 1f\n"
        "    xor %ebp, %ebp\n"
        "    pop %rax\n"
        "    call *%rax\n"
        "    xor %edi, %edi\n"
        "    mov $60, %eax\n"
        "    syscall\n"
        "    hlt\n"
        "1:  ret\n");

#define TUNIX_START(entry)                  \
    __asm__(".text\n"                       \
            ".globl _start\n"               \
            "_start:\n"                     \
            "    xor %ebp, %ebp\n"          \
            "    and $-16, %rsp\n"          \
            "    call " #entry "\n"         \
            "    hlt\n");

#elif defined(__aarch64__)

#define TUNIX_AT_FDCWD -100
#define TUNIX_SIGCHLD 17

extern s64 tunix_svc(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f);
__asm__(".text\n"
        ".globl tunix_svc\n"
        "tunix_svc:\n"
        "    mov x8, x0\n"
        "    mov x0, x1\n"
        "    mov x1, x2\n"
        "    mov x2, x3\n"
        "    mov x3, x4\n"
        "    mov x4, x5\n"
        "    mov x5, x6\n"
        "    svc #0\n"
        "    ret\n");

static inline s64 syscall6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
    switch (n) {
    case 0: return tunix_svc(63, a, b, c, d, e, f);
    case 1: return tunix_svc(64, a, b, c, d, e, f);
    case 2: return tunix_svc(56, TUNIX_AT_FDCWD, a, b, c, 0, 0);
    case 3: return tunix_svc(57, a, b, c, d, e, f);
    case 17: return tunix_svc(67, a, b, c, d, e, f);
    case 18: return tunix_svc(68, a, b, c, d, e, f);
    case 19: return tunix_svc(65, a, b, c, d, e, f);
    case 20: return tunix_svc(66, a, b, c, d, e, f);
    case 8: return tunix_svc(62, a, b, c, d, e, f);
    case 9: return tunix_svc(222, a, b, c, d, e, f);
    case 11: return tunix_svc(215, a, b, c, d, e, f);
    case 16: return tunix_svc(29, a, b, c, d, e, f);
    case 22: return tunix_svc(59, a, 0, 0, 0, 0, 0);
    case 26: return tunix_svc(227, a, b, c, d, e, f);
    case 32: return tunix_svc(23, a, b, c, d, e, f);
    case 35: return tunix_svc(101, a, b, c, d, e, f);
    case 39: return tunix_svc(172, a, b, c, d, e, f);
    case 41: return tunix_svc(198, a, b, c, d, e, f);
    case 42: return tunix_svc(203, a, b, c, d, e, f);
    case 43: return tunix_svc(202, a, b, c, d, e, f);
    case 53: return tunix_svc(199, a, b, c, d, e, f);
    case 49: return tunix_svc(200, a, b, c, d, e, f);
    case 50: return tunix_svc(201, a, b, c, d, e, f);
    case 56: return tunix_svc(220, a, b, c, e, d, 0);
    case 57: return tunix_svc(220, TUNIX_SIGCHLD, 0, 0, 0, 0, 0);
    case 59: return tunix_svc(221, a, b, c, d, e, f);
    case 60: return tunix_svc(93, a, b, c, d, e, f);
    case 61: return tunix_svc(260, a, b, c, d, e, f);
    case 62: return tunix_svc(129, a, b, c, d, e, f);
    case 74: return tunix_svc(82, a, b, c, d, e, f);
    case 77: return tunix_svc(46, a, b, c, d, e, f);
    case 87: return tunix_svc(35, TUNIX_AT_FDCWD, a, 0, 0, 0, 0);
    case 99: return tunix_svc(179, a, b, c, d, e, f);
    case 103: return tunix_svc(116, a, b, c, d, e, f);
    case 117: return tunix_svc(147, a, b, c, d, e, f);
    case 313: return tunix_svc(273, a, b, c, d, e, f);
    case 141: return tunix_svc(140, a, b, c, d, e, f);
    case 169: return tunix_svc(142, a, b, c, d, e, f);
    case 202: return tunix_svc(98, a, b, c, d, e, f);
    case 203: return tunix_svc(122, a, b, c, d, e, f);
    case 228: return tunix_svc(113, a, b, c, d, e, f);
    case 231: return tunix_svc(94, a, b, c, d, e, f);
    case 257: return tunix_svc(56, a, b, c, d, e, f);
    default: return -38;
    }
}

static inline s64 syscall0(s64 n) { return syscall6(n, 0, 0, 0, 0, 0, 0); }
static inline s64 syscall1(s64 n, s64 a) { return syscall6(n, a, 0, 0, 0, 0, 0); }
static inline s64 syscall2(s64 n, s64 a, s64 b) { return syscall6(n, a, b, 0, 0, 0, 0); }
static inline s64 syscall3(s64 n, s64 a, s64 b, s64 c) { return syscall6(n, a, b, c, 0, 0, 0); }
static inline s64 syscall4(s64 n, s64 a, s64 b, s64 c, s64 d) { return syscall6(n, a, b, c, d, 0, 0); }

__asm__(".text\n"
        ".globl spawn_thread\n"
        "spawn_thread:\n"
        "    mov x2, #0\n"
        "    mov x3, #0\n"
        "    mov x4, #0\n"
        "    mov x8, #220\n"
        "    svc #0\n"
        "    cbnz x0, 1f\n"
        "    ldr x9, [sp], #8\n"
        "    mov x29, #0\n"
        "    blr x9\n"
        "    mov x0, #0\n"
        "    mov x8, #93\n"
        "    svc #0\n"
        "    b .\n"
        "1:  ret\n");

#define TUNIX_START(entry)                  \
    __asm__(".text\n"                       \
            ".globl _start\n"               \
            "_start:\n"                     \
            "    mov x29, #0\n"             \
            "    bl " #entry "\n"           \
            "    b .\n");

#endif

#endif
