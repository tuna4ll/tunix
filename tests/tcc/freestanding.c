static long syscall1(long number, long first) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(first)
                     : "rcx", "r11", "memory");
    return result;
}

static long syscall3(long number, long first, long second, long third) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(first), "S"(second), "d"(third)
                     : "rcx", "r11", "memory");
    return result;
}

void _start(void) {
    static const char message[] = "tcc smoke ok\n";
    syscall3(1, 1, (long)message, sizeof(message) - 1);
    syscall1(60, 0);
    for (;;) { }
}
