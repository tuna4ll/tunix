// A freestanding AArch64 program that reports the initial stack the kernel
// built for it: argc, argv, envp and the auxiliary vector.

__asm__(".global _start\n"
        "_start:\n"
        "  mov x0, sp\n"
        "  b   start_c\n");

#define SYS_WRITE 64
#define SYS_EXIT  93

static long call3(long number, long a, long b, long c) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static unsigned long string_length(const char *text) {
    unsigned long length = 0;
    while (text[length]) length++;
    return length;
}

static void put(const char *text) {
    call3(SYS_WRITE, 1, (long)text, (long)string_length(text));
}

static void put_unsigned(unsigned long value, unsigned base) {
    char digits[24];
    int count = 0;
    if (!value) digits[count++] = '0';
    while (value) {
        unsigned long digit = value % base;
        digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= base;
    }
    char out[24];
    int length = 0;
    while (count) out[length++] = digits[--count];
    call3(SYS_WRITE, 1, (long)out, length);
}

void start_c(unsigned long *stack) {
    unsigned long argc = stack[0];
    char **argv = (char **)&stack[1];

    put("initargs: argc=");
    put_unsigned(argc, 10);
    put("\n");

    for (unsigned long i = 0; i < argc; i++) {
        put("initargs: argv[");
        put_unsigned(i, 10);
        put("]=");
        put(argv[i] ? argv[i] : "(null)");
        put("\n");
    }

    char **envp = argv + argc + 1;              // past the argv NULL
    unsigned long envc = 0;
    while (envp[envc]) envc++;

    put("initargs: envc=");
    put_unsigned(envc, 10);
    put("\n");
    for (unsigned long i = 0; i < envc; i++) {
        put("initargs: env ");
        put(envp[i]);
        put("\n");
    }

    unsigned long *auxv = (unsigned long *)(envp + envc + 1);
    unsigned long entries = 0;
    for (; auxv[0]; auxv += 2) {
        put("initargs: auxv type=");
        put_unsigned(auxv[0], 10);
        put(" value=0x");
        put_unsigned(auxv[1], 16);
        put("\n");
        entries++;
    }

    put("initargs: auxv entries=");
    put_unsigned(entries, 10);
    put("\nINITARGS DONE\n");

    call3(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}
