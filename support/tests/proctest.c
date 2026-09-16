typedef unsigned long u64;
typedef long s64;

#if defined(__x86_64__)
#define NR_READ 0
#define NR_WRITE 1
#define NR_RT_SIGACTION 13
#define NR_SCHED_YIELD 24
#define NR_NANOSLEEP 35
#define NR_GETPID 39
#define NR_CLONE 56
#define NR_EXECVE 59
#define NR_EXIT 60
#define NR_WAIT4 61
#define NR_KILL 62
#define NR_ARCH_PRCTL 158
#define NR_OPENAT 257
#define NR_PIPE2 293
#define NR_PPOLL 271
#define NR_PSELECT6 270
#define NR_EPOLL_CREATE1 291
#define NR_EPOLL_PWAIT 281
#define NR_CLOCK_GETTIME 228
#define NR_EPOLL_CTL 233
#define NR_GETRUSAGE 98
#define NR_SOCKETPAIR 53
#define EPOLL_PACKED __attribute__((packed))
#define UCONTEXT_RET_OFFSET (40 + 13 * 8)
#define TRAP_LENGTH 2
#define UCONTEXT_IP_OFFSET (40 + 16 * 8)
#elif defined(__aarch64__)
#define NR_READ 63
#define NR_WRITE 64
#define NR_RT_SIGACTION 134
#define NR_SCHED_YIELD 124
#define NR_NANOSLEEP 101
#define NR_GETPID 172
#define NR_CLONE 220
#define NR_EXECVE 221
#define NR_EXIT 93
#define NR_WAIT4 260
#define NR_KILL 129
#define NR_OPENAT 56
#define NR_PIPE2 59
#define NR_PPOLL 73
#define NR_PSELECT6 72
#define NR_EPOLL_CREATE1 20
#define NR_EPOLL_PWAIT 22
#define NR_CLOCK_GETTIME 113
#define NR_EPOLL_CTL 21
#define NR_GETRUSAGE 165
#define NR_SOCKETPAIR 199
#define EPOLL_PACKED
#define UCONTEXT_RET_OFFSET (168 + 8)
#define TRAP_LENGTH 4
#define UCONTEXT_IP_OFFSET (168 + 264)
#endif

#define AT_FDCWD -100
#define O_WRONLY 1
#define SIGCHLD 17
#define SIGILL 4
#define SIGUSR1 10
#define SIGUSR2 12
#define EINTR 4
#define O_NONBLOCK 04000
#define EPOLLIN 0x001U
#define EPOLLET 0x80000000U
#define EPOLL_CTL_ADD 1
#define SA_SIGINFO 0x00000004UL
#define SA_RESTORER 0x04000000UL
#define SA_RESTART 0x10000000UL
#define CLONE_VM 0x00000100UL
#define CLONE_FS 0x00000200UL
#define CLONE_FILES 0x00000400UL
#define CLONE_SIGHAND 0x00000800UL
#define CLONE_THREAD 0x00010000UL
#define CLONE_SETTLS 0x00080000UL

struct sigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

struct timespec {
    s64 seconds;
    s64 nanoseconds;
};

extern s64 sys(u64 number, u64 a, u64 b, u64 c, u64 d, u64 e);
extern s64 sys6(u64 number, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f);
extern void restorer(void);
extern s64 trap_site(void);
extern s64 clone_thread(u64 flags, u64 stack, u64 tls, s64 (*entry)(void));

#if defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "    xor %rbp, %rbp\n"
    "    mov %rsp, %rdi\n"
    "    and $-16, %rsp\n"
    "    call start_c\n"
    "    hlt\n"
    ".globl sys\n"
    "sys:\n"
    "    mov %rdi, %rax\n"
    "    mov %rsi, %rdi\n"
    "    mov %rdx, %rsi\n"
    "    mov %rcx, %rdx\n"
    "    mov %r8, %r10\n"
    "    mov %r9, %r8\n"
    "    syscall\n"
    "    ret\n"
    ".globl sys6\n"
    "sys6:\n"
    "    mov %rdi, %rax\n"
    "    mov %rsi, %rdi\n"
    "    mov %rdx, %rsi\n"
    "    mov %rcx, %rdx\n"
    "    mov %r8, %r10\n"
    "    mov %r9, %r8\n"
    "    mov 8(%rsp), %r9\n"
    "    syscall\n"
    "    ret\n"
    ".globl restorer\n"
    "restorer:\n"
    "    mov $15, %eax\n"
    "    syscall\n"
    "    hlt\n"
    ".globl trap_site\n"
    "trap_site:\n"
    "    ud2\n"
    "    mov $77, %eax\n"
    "    ret\n"
    ".globl clone_thread\n"
    "clone_thread:\n"
    "    mov %rcx, %r9\n"
    "    mov %rdx, %r8\n"
    "    xor %edx, %edx\n"
    "    xor %r10d, %r10d\n"
    "    mov $56, %eax\n"
    "    syscall\n"
    "    test %rax, %rax\n"
    "    jnz 1f\n"
    "    xor %ebp, %ebp\n"
    "    call *%r9\n"
    "    mov %rax, %rdi\n"
    "    mov $60, %eax\n"
    "    syscall\n"
    "    hlt\n"
    "1:  ret\n");
#elif defined(__aarch64__)
__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "    mov x29, #0\n"
    "    mov x0, sp\n"
    "    bl start_c\n"
    "    b .\n"
    ".globl sys\n"
    "sys:\n"
    "    mov x8, x0\n"
    "    mov x0, x1\n"
    "    mov x1, x2\n"
    "    mov x2, x3\n"
    "    mov x3, x4\n"
    "    mov x4, x5\n"
    "    svc #0\n"
    "    ret\n"
    ".globl sys6\n"
    "sys6:\n"
    "    mov x8, x0\n"
    "    mov x0, x1\n"
    "    mov x1, x2\n"
    "    mov x2, x3\n"
    "    mov x3, x4\n"
    "    mov x4, x5\n"
    "    mov x5, x6\n"
    "    svc #0\n"
    "    ret\n"
    ".globl restorer\n"
    "restorer:\n"
    "    mov x8, #139\n"
    "    svc #0\n"
    "    b .\n"
    ".globl trap_site\n"
    "trap_site:\n"
    "    udf #0\n"
    "    mov x0, #77\n"
    "    ret\n"
    ".globl clone_thread\n"
    "clone_thread:\n"
    "    mov x10, x3\n"
    "    mov x3, x2\n"
    "    mov x2, #0\n"
    "    mov x4, #0\n"
    "    mov x8, #220\n"
    "    svc #0\n"
    "    cbnz x0, 1f\n"
    "    mov x29, #0\n"
    "    blr x10\n"
    "    mov x8, #93\n"
    "    svc #0\n"
    "    b .\n"
    "1:  ret\n");
#endif

static s64 console = 1;
static int failures;

static u64 length_of(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    return length;
}

static void say(const char *text) {
    sys(NR_WRITE, (u64)console, (u64)text, length_of(text), 0, 0);
}

static void say_number(s64 value) {
    char digits[24];
    int at = 23;
    int negative = value < 0;
    u64 magnitude = negative ? (u64)-value : (u64)value;
    digits[at] = 0;
    do {
        digits[--at] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (negative) digits[--at] = '-';
    say(digits + at);
}

static void check(const char *name, int ok, s64 detail) {
    say("PROCTEST ");
    say(name);
    if (ok) {
        say(" ok\n");
        return;
    }
    say(" FAIL ");
    say_number(detail);
    say("\n");
    failures++;
}

static void sleep_ms(s64 milliseconds) {
    struct timespec delay;
    delay.seconds = milliseconds / 1000;
    delay.nanoseconds = (milliseconds % 1000) * 1000000;
    sys(NR_NANOSLEEP, (u64)&delay, 0, 0, 0, 0);
}

static s64 fork_process(void) {
    return sys(NR_CLONE, SIGCHLD, 0, 0, 0, 0);
}

static s64 wait_child(s64 pid) {
    int status = 0;
    s64 got = sys(NR_WAIT4, (u64)pid, (u64)&status, 0, 0, 0);
    if (got != pid) return -1000 - got;
    return status;
}

static void exit_now(int status) {
    sys(NR_EXIT, (u64)status, 0, 0, 0, 0);
    for (;;) {
    }
}

static u64 tls_read(void) {
    u64 value;
#if defined(__x86_64__)
    __asm__ volatile("mov %%fs:0, %0" : "=r"(value));
#else
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
#endif
    return value;
}

static void tls_write(u64 *block) {
    block[0] = (u64)block;
#if defined(__x86_64__)
    sys(NR_ARCH_PRCTL, 0x1002, (u64)block, 0, 0, 0);
#else
    __asm__ volatile("msr tpidr_el0, %0" : : "r"((u64)block));
#endif
}

static void fpu_write(u64 value) {
#if defined(__x86_64__)
    __asm__ volatile("movq %0, %%xmm0" : : "r"(value));
#else
    __asm__ volatile("fmov d0, %0" : : "r"(value));
#endif
}

static u64 fpu_read(void) {
    u64 value;
#if defined(__x86_64__)
    __asm__ volatile("movq %%xmm0, %0" : "=r"(value));
#else
    __asm__ volatile("fmov %0, d0" : "=r"(value));
#endif
    return value;
}

static int state_survives_switches(u64 *block, u64 pattern) {
    tls_write(block);
    for (int round = 0; round < 400; round++) {
        fpu_write(pattern + (u64)round);
        sys(NR_SCHED_YIELD, 0, 0, 0, 0, 0);
        if (fpu_read() != pattern + (u64)round) return 1;
        if (tls_read() != (u64)block) return 2;
    }
    return 0;
}

static void test_fork_and_switches(void) {
    static u64 parent_block[4];
    static u64 child_block[4];
    s64 pid = fork_process();
    if (pid == 0) exit_now(state_survives_switches(child_block, 0x1111222233334444UL));
    check("fork", pid > 0, pid);
    if (pid <= 0) return;
    int mine = state_survives_switches(parent_block, 0x5555666677778888UL);
    check("parent tls+fpu across switches", mine == 0, mine);
    s64 status = wait_child(pid);
    check("child tls+fpu across switches", status == 0, status);
}

static unsigned char thread_stack[65536] __attribute__((aligned(16)));
static u64 thread_block[4];
static volatile int thread_result = -1;

static s64 thread_main(void) {
    u64 marker = 0;
    u64 here = (u64)&marker;
    int ok = tls_read() == (u64)thread_block &&
             here > (u64)thread_stack && here < (u64)thread_stack + sizeof(thread_stack);
    thread_result = ok ? 1 : 0;
    return 0;
}

static void test_thread(void) {
    thread_block[0] = (u64)thread_block;
    u64 flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS;
    s64 tid = clone_thread(flags, (u64)thread_stack + sizeof(thread_stack),
                           (u64)thread_block, thread_main);
    check("clone thread", tid > 0, tid);
    if (tid <= 0) return;
    for (int spin = 0; spin < 2000 && thread_result < 0; spin++)
        sys(NR_SCHED_YIELD, 0, 0, 0, 0, 0);
    check("thread tls and stack", thread_result == 1, thread_result);
}

static volatile int handler_signal;
static volatile int handler_info_signal;
static volatile u64 handler_ip;

static void siginfo_handler(int signal_number, int *info, void *context) {
    handler_signal = signal_number;
    handler_info_signal = info ? info[0] : -1;
    handler_ip = *(u64 *)((char *)context + UCONTEXT_IP_OFFSET);
    *(u64 *)((char *)context + UCONTEXT_RET_OFFSET) = 0x5157;
}

static void test_siginfo(void) {
    struct sigaction action;
    action.handler = (u64)siginfo_handler;
    action.flags = SA_SIGINFO | SA_RESTORER;
    action.restorer = (u64)restorer;
    action.mask = 0;
    s64 installed = sys(NR_RT_SIGACTION, SIGUSR1, (u64)&action, 0, 8, 0);
    check("sigaction", installed == 0, installed);
    s64 pid = sys(NR_GETPID, 0, 0, 0, 0, 0);
    s64 result = sys(NR_KILL, (u64)pid, SIGUSR1, 0, 0, 0);
    check("handler arguments", handler_signal == SIGUSR1 && handler_info_signal == SIGUSR1,
          handler_signal * 100 + handler_info_signal);
    check("context ip", handler_ip != 0, 0);
    check("sigreturn reads the context", result == 0x5157, result);
}

static volatile u64 trap_ip;

static void trap_handler(int signal_number, int *info, void *context) {
    (void)signal_number;
    (void)info;
    u64 *ip = (u64 *)((char *)context + UCONTEXT_IP_OFFSET);
    trap_ip = *ip;
    *ip += TRAP_LENGTH;
}

static void test_trap_resume(void) {
    struct sigaction action;
    action.handler = (u64)trap_handler;
    action.flags = SA_SIGINFO | SA_RESTORER;
    action.restorer = (u64)restorer;
    action.mask = 0;
    sys(NR_RT_SIGACTION, SIGILL, (u64)&action, 0, 8, 0);
    s64 result = trap_site();
    check("illegal instruction resumes where the handler says",
          result == 77 && trap_ip == (u64)trap_site, result);
}

static volatile int interrupts;

static void count_handler(int signal_number) {
    (void)signal_number;
    interrupts++;
}

static void test_blocked_read(const char *name, u64 extra_flags, s64 expected_first) {
    struct sigaction action;
    action.handler = (u64)count_handler;
    action.flags = SA_RESTORER | extra_flags;
    action.restorer = (u64)restorer;
    action.mask = 0;
    sys(NR_RT_SIGACTION, SIGUSR2, (u64)&action, 0, 8, 0);

    int fds[2] = { -1, -1 };
    s64 made = sys(NR_PIPE2, (u64)fds, 0, 0, 0, 0);
    if (made != 0) {
        check(name, 0, made);
        return;
    }
    s64 parent = sys(NR_GETPID, 0, 0, 0, 0, 0);
    s64 pid = fork_process();
    if (pid == 0) {
        sleep_ms(150);
        sys(NR_KILL, (u64)parent, SIGUSR2, 0, 0, 0);
        sleep_ms(150);
        sys(NR_WRITE, (u64)fds[1], (u64)"z", 1, 0, 0);
        exit_now(0);
    }
    interrupts = 0;
    char byte = 0;
    s64 first = sys(NR_READ, (u64)fds[0], (u64)&byte, 1, 0, 0);
    int ok = first == expected_first && interrupts == 1;
    if (first < 0) {
        s64 second = sys(NR_READ, (u64)fds[0], (u64)&byte, 1, 0, 0);
        ok = ok && second == 1 && byte == 'z';
    } else {
        ok = ok && byte == 'z';
    }
    check(name, ok, first);
    wait_child(pid);
}

static s64 monotonic_ms(void) {
    struct timespec now;
    sys(NR_CLOCK_GETTIME, 1, (u64)&now, 0, 0, 0);
    return now.seconds * 1000 + now.nanoseconds / 1000000;
}

static void check_wait(const char *name, s64 started, s64 result) {
    s64 elapsed = monotonic_ms() - started;
    check(name, result == 0 && elapsed >= 250 && elapsed < 3000, result ? result : elapsed);
}

static void test_timeouts(void) {
    struct timespec wait;
    wait.seconds = 0;
    wait.nanoseconds = 300000000;
    s64 started = monotonic_ms();
    check_wait("ppoll waits for its timeout", started, sys6(NR_PPOLL, 0, 0, (u64)&wait, 0, 8, 0));

    wait.seconds = 0;
    wait.nanoseconds = 300000000;
    started = monotonic_ms();
    check_wait("pselect6 waits for its timeout", started,
               sys6(NR_PSELECT6, 0, 0, 0, 0, (u64)&wait, 0));

    s64 epoll = sys(NR_EPOLL_CREATE1, 0, 0, 0, 0, 0);
    char events[64];
    started = monotonic_ms();
    check_wait("epoll_pwait waits for its timeout", started,
               epoll < 0 ? epoll : sys6(NR_EPOLL_PWAIT, (u64)epoll, (u64)events, 4, 300, 0, 8));

    int fds[2] = { -1, -1 };
    sys(NR_PIPE2, (u64)fds, 0, 0, 0, 0);
    struct { int fd; short events; short revents; } poller = { fds[0], 1, 0 };
    wait.seconds = 0;
    wait.nanoseconds = 300000000;
    started = monotonic_ms();
    check_wait("ppoll on an idle pipe waits", started, sys6(NR_PPOLL, (u64)&poller, 1, (u64)&wait, 0, 8, 0));
}

struct epoll_event_abi {
    unsigned int events;
    u64 data;
} EPOLL_PACKED;

static s64 epoll_now(s64 epoll, struct epoll_event_abi *out) {
    return sys6(NR_EPOLL_PWAIT, (u64)epoll, (u64)out, 4, 0, 0, 8);
}

static void test_edge_triggered(void) {
    int fds[2] = { -1, -1 };
    s64 made = sys(NR_PIPE2, (u64)fds, O_NONBLOCK, 0, 0, 0);
    s64 edge = sys(NR_EPOLL_CREATE1, 0, 0, 0, 0, 0);
    s64 level = sys(NR_EPOLL_CREATE1, 0, 0, 0, 0, 0);
    if (made != 0 || edge < 0 || level < 0) {
        check("epoll setup", 0, made ? made : edge);
        return;
    }
    struct epoll_event_abi interest = { EPOLLIN | EPOLLET, 7 };
    sys(NR_EPOLL_CTL, (u64)edge, EPOLL_CTL_ADD, (u64)fds[0], (u64)&interest, 0);
    interest.events = EPOLLIN;
    sys(NR_EPOLL_CTL, (u64)level, EPOLL_CTL_ADD, (u64)fds[0], (u64)&interest, 0);
    struct epoll_event_abi out[4];

    sys(NR_WRITE, (u64)fds[1], (u64)"ab", 2, 0, 0);
    s64 first = epoll_now(edge, out);
    u64 data = out[0].data;
    s64 again = epoll_now(edge, out);
    check("epoll edge reported once", first == 1 && data == 7 && again == 0, first * 10 + again);

    s64 steady = epoll_now(level, out) + epoll_now(level, out);
    check("epoll level stays ready", steady == 2, steady);

    char buffer[8];
    while (sys(NR_READ, (u64)fds[0], (u64)buffer, sizeof(buffer), 0, 0) > 0) {
    }
    s64 idle = epoll_now(edge, out);
    sys(NR_WRITE, (u64)fds[1], (u64)"c", 1, 0, 0);
    s64 rearmed = epoll_now(edge, out);
    check("epoll edge rearms after EAGAIN", idle == 0 && rearmed == 1, idle * 10 + rearmed);
}

static void test_idle_poll_sleeps(void) {
    int fds[2] = { -1, -1 };
    sys(NR_PIPE2, (u64)fds, 0, 0, 0, 0);
    struct { int fd; short events; short revents; } poller = { fds[0], 1, 0 };
    u64 before[18];
    u64 after[18];
    sys(NR_GETRUSAGE, 0, (u64)before, 0, 0, 0);
    struct timespec wait;
    wait.seconds = 1;
    wait.nanoseconds = 0;
    sys6(NR_PPOLL, (u64)&poller, 1, (u64)&wait, 0, 8, 0);
    sys(NR_GETRUSAGE, 0, (u64)after, 0, 0, 0);
    s64 switches = (s64)(after[16] - before[16]);
    check("idle poll is not woken every tick", switches >= 1 && switches < 20, switches);

    u64 readable[16] = { 0 };
    readable[fds[0] / 64] |= 1UL << (fds[0] % 64);
    wait.seconds = 1;
    wait.nanoseconds = 0;
    sys(NR_GETRUSAGE, 0, (u64)before, 0, 0, 0);
    sys6(NR_PSELECT6, (u64)fds[0] + 1, (u64)readable, 0, 0, (u64)&wait, 0);
    sys(NR_GETRUSAGE, 0, (u64)after, 0, 0, 0);
    switches = (s64)(after[16] - before[16]);
    check("idle select is not woken every tick", switches >= 1 && switches < 20, switches);

    int pair[2] = { -1, -1 };
    sys(NR_SOCKETPAIR, 1, 1, 0, (u64)pair, 0);
    s64 pid = fork_process();
    if (pid == 0) {
        sleep_ms(1000);
        sys(NR_WRITE, (u64)pair[1], (u64)"s", 1, 0, 0);
        exit_now(0);
    }
    char byte = 0;
    sys(NR_GETRUSAGE, 0, (u64)before, 0, 0, 0);
    s64 got = sys(NR_READ, (u64)pair[0], (u64)&byte, 1, 0, 0);
    sys(NR_GETRUSAGE, 0, (u64)after, 0, 0, 0);
    switches = (s64)(after[16] - before[16]);
    wait_child(pid);
    check("blocked socket read is not woken every tick",
          got == 1 && byte == 's' && switches >= 1 && switches < 20, got == 1 ? switches : got);
}

static int text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void finish(void) {
    say(failures ? "PROCTEST FAIL\n" : "PROCTEST PASS\n");
    say("PROCTEST DONE\n");
    for (;;) sleep_ms(1000);
}

void start_c(u64 *stack) {
    s64 argc = (s64)stack[0];
    char **argv = (char **)(stack + 1);
    s64 fd = sys(NR_OPENAT, (u64)AT_FDCWD, (u64)"/dev/console", O_WRONLY, 0, 0);
    if (fd >= 0) console = fd;

    if (argc >= 2 && text_equal(argv[1], "exec")) {
        check("execve", 1, 0);
        finish();
    }

    test_timeouts();
    test_edge_triggered();
    test_idle_poll_sleeps();
    test_fork_and_switches();
    test_thread();
    test_siginfo();
    test_trap_resume();
    test_blocked_read("read interrupted without SA_RESTART", 0, -EINTR);
    test_blocked_read("read restarted with SA_RESTART", SA_RESTART, 1);

    if (failures) finish();
    char *exec_argv[] = { argv[0], (char *)"exec", 0 };
    char *exec_envp[] = { 0 };
    s64 result = sys(NR_EXECVE, (u64)argv[0], (u64)exec_argv, (u64)exec_envp, 0, 0);
    check("execve", 0, result);
    finish();
}
