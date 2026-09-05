/* The scheduler, measured from inside the machine it schedules. */
/* The numbers in docs/syscalls-and-scheduler.md came from a program built
   against the kernel's own libc, and that libc and that program are both
   gone. */
/* This is the replacement, and it is freestanding on purpose so that what it
   measures is the kernel with no libc between them. */
/* It runs as init, and every test prints one line beginning with a tag a script
   can grep for. */
/* The last line is BENCH DONE, which is what the harness waits for before it
   takes the machine down. */

typedef unsigned long u64;
typedef long s64;

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_pipe 22
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_clone 56
#define SYS_setpriority 141
#define SYS_sched_setaffinity 203
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_reboot 169

#define CLONE_VM 0x00000100UL
#define CLONE_FS 0x00000200UL
#define CLONE_FILES 0x00000400UL
#define CLONE_SIGHAND 0x00000800UL
#define CLONE_THREAD 0x00010000UL
#define THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)

#define CLOCK_MONOTONIC 1
#define SIGKILL 9

struct timespec {
    s64 seconds;
    s64 nanoseconds;
};

static inline s64 syscall1(s64 number, s64 a) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a)
                     : "rcx", "r11", "memory");
    return result;
}

static inline s64 syscall2(s64 number, s64 a, s64 b) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return result;
}

static inline s64 syscall3(s64 number, s64 a, s64 b, s64 c) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}

static inline s64 syscall4(s64 number, s64 a, s64 b, s64 c, s64 d) {
    s64 result;
    register s64 r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

/* The child comes back on a stack of its own, which is not something a C
   function can be made to do, so the syscall itself is written out here. */
extern s64 spawn_thread(u64 flags, void *child_stack_top);
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

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
}

/* Three digits of fraction is what makes a microsecond legible in a
   millisecond column. */
static void put_fixed(u64 value, unsigned fraction_digits) {
    char buffer[32];
    int index = (int)sizeof(buffer);
    buffer[--index] = 0;
    unsigned digits = 0;
    do {
        buffer[--index] = (char)('0' + value % 10);
        value /= 10;
        digits++;
        if (digits == fraction_digits) buffer[--index] = '.';
    } while (value || digits < fraction_digits + 1);
    put(buffer + index);
}

static void put_number(u64 value) { put_fixed(value, 0); }

static u64 now_ns(void) {
    struct timespec value = {0, 0};
    (void)syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.seconds * 1000000000UL + (u64)value.nanoseconds;
}

static void sleep_ns(u64 nanoseconds) {
    struct timespec request;
    request.seconds = (s64)(nanoseconds / 1000000000UL);
    request.nanoseconds = (s64)(nanoseconds % 1000000000UL);
    (void)syscall2(SYS_nanosleep, (s64)&request, 0);
}

static void pin_to_cpu(unsigned cpu) {
    u64 mask = 1UL << cpu;
    (void)syscall3(SYS_sched_setaffinity, 0, sizeof(mask), (s64)&mask);
}

/* A mask fork hands on, so a test that pinned itself has to give the whole
   machine back before the next one forks its workers. */
static void pin_to_all(unsigned cpus) {
    u64 mask = cpus >= 64 ? ~0UL : (1UL << cpus) - 1UL;
    (void)syscall3(SYS_sched_setaffinity, 0, sizeof(mask), (s64)&mask);
}

static void sort_ascending(u64 *values, unsigned count) {
    for (unsigned i = 1; i < count; i++) {
        u64 key = values[i];
        unsigned j = i;
        while (j && values[j - 1] > key) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = key;
    }
}

/* Work the compiler cannot fold away, so that a spinner really does spend the
   processor it was given. */
static volatile u64 sink;

/* One round of arithmetic and nothing else, so that a spinner spends the
   processor rather than the kernel lock every syscall takes. */
static void spin_round(void) {
    u64 accumulator = sink;
    for (unsigned i = 0; i < 2048; i++) accumulator = accumulator * 6364136223846793005UL + 1442695040888963407UL;
    sink = accumulator;
}

/* The clock is read once every 64 rounds rather than every one, because reading
   it is a syscall, every syscall takes the one kernel lock, and a loop that took
   it that often would measure the lock instead of the processor. */
static u64 spin_for(u64 nanoseconds) {
    u64 deadline = now_ns() + nanoseconds;
    u64 rounds = 0;
    for (;;) {
        for (unsigned i = 0; i < 64; i++) spin_round();
        rounds += 64;
        if (now_ns() >= deadline) return rounds;
    }
}

static void spin_rounds(u64 rounds) {
    while (rounds--) spin_round();
}

static void spin_forever(void) {
    for (;;) spin_round();
}

/* Two equal-weight children, one reniced, and the ratio of the work they got
   through. */
/* The weights are 1024 and 110, so the answer the scheduler owes is 9.31 and
   one that ignores nice answers 1. */
static void test_nice_ratio(u64 duration_ns) {
    int channel[2];
    if (syscall1(SYS_pipe, (s64)channel) != 0) { put("NICE fail pipe\n"); return; }

    for (int which = 0; which < 2; which++) {
        s64 child = syscall1(SYS_fork, 0);
        if (child == 0) {
            (void)syscall1(SYS_close, channel[0]);
            pin_to_cpu(0);
            if (which) (void)syscall3(SYS_setpriority, 0, 0, 10);
            u64 rounds = spin_for(duration_ns);
            u64 message[2] = {(u64)which, rounds};
            (void)syscall3(SYS_write, channel[1], (s64)message, sizeof(message));
            (void)syscall1(SYS_exit_group, 0);
        }
    }
    (void)syscall1(SYS_close, channel[1]);

    u64 rounds[2] = {0, 0};
    for (int received = 0; received < 2; received++) {
        u64 message[2];
        s64 got = syscall3(SYS_read, channel[0], (s64)message, sizeof(message));
        if (got != (s64)sizeof(message)) { put("NICE fail read\n"); break; }
        rounds[message[0]] = message[1];
    }
    (void)syscall1(SYS_close, channel[0]);
    (void)syscall4(SYS_wait4, -1, 0, 0, 0);
    (void)syscall4(SYS_wait4, -1, 0, 0, 0);

    put("NICE nice0=");
    put_number(rounds[0]);
    put(" nice10=");
    put_number(rounds[1]);
    put(" ratio=");
    put_fixed(rounds[1] ? rounds[0] * 100UL / rounds[1] : 0, 2);
    put("\n");
}

/* How late a thread that asked to wake every 20 ms actually woke, with the
   processors already full. */
/* This is the number a quantum longer than it should be shows up in, and the
   one an audio period cares about. */
static void test_wake_latency(unsigned spinners, unsigned samples, unsigned cpus) {
    static u64 lateness[512];
    if (samples > 512) samples = 512;

    s64 children[16];
    unsigned started = 0;
    for (unsigned i = 0; i < spinners && i < 16; i++) {
        s64 child = syscall1(SYS_fork, 0);
        if (child == 0) {
            pin_to_cpu(i % cpus);
            spin_forever();
        }
        if (child > 0) children[started++] = child;
    }

    /* Let them all be running before the first sample, or the first few measure
       an empty machine. */
    sleep_ns(200000000UL);

    const u64 period = 20000000UL;
    for (unsigned i = 0; i < samples; i++) {
        u64 before = now_ns();
        sleep_ns(period);
        u64 elapsed = now_ns() - before;
        lateness[i] = elapsed > period ? elapsed - period : 0;
    }

    for (unsigned i = 0; i < started; i++) (void)syscall2(SYS_kill, children[i], SIGKILL);
    for (unsigned i = 0; i < started; i++) (void)syscall4(SYS_wait4, children[i], 0, 0, 0);

    sort_ascending(lateness, samples);
    put("WAKE spinners=");
    put_number(spinners);
    put(" median=");
    put_fixed(lateness[samples / 2] / 1000UL, 3);
    put("ms p95=");
    put_fixed(lateness[samples * 95 / 100] / 1000UL, 3);
    put("ms max=");
    put_fixed(lateness[samples - 1] / 1000UL, 3);
    put("ms\n");
}

static int pingpong_up[2];
static int pingpong_down[2];
static u64 pingpong_rounds;

static void pingpong_partner(void) {
    char byte = 0;
    for (u64 i = 0; i < pingpong_rounds; i++) {
        if (syscall3(SYS_read, pingpong_up[0], (s64)&byte, 1) != 1) break;
        if (syscall3(SYS_write, pingpong_down[1], (s64)&byte, 1) != 1) break;
    }
}

/* The cost of handing the processor over and getting it back, measured once
   between two threads of one process and once between two processes. */
/* The pair is the point, because both switch and both go through the same
   scheduler while only the second changes address space. */
/* So the difference between them is what the page tables cost, which is the
   whole of what a reload of CR3 that did not have to happen would add to the
   first. */
static void test_switch_cost(int threaded, u64 rounds, unsigned cpus) {
    static char thread_stack[65536] __attribute__((aligned(16)));
    if (syscall1(SYS_pipe, (s64)pingpong_up) != 0) return;
    if (syscall1(SYS_pipe, (s64)pingpong_down) != 0) return;
    pingpong_rounds = rounds;

    /* Both ends on one processor, so this measures a context switch rather than
       how fast two processors can pass a byte between them. */
    pin_to_cpu(0);

    s64 partner;
    if (threaded) {
        char *top = thread_stack + sizeof(thread_stack);
        top -= 8;
        *(void **)top = (void *)pingpong_partner;
        partner = spawn_thread(THREAD_FLAGS, top);
    } else {
        partner = syscall1(SYS_fork, 0);
        if (partner == 0) {
            pin_to_cpu(0);
            pingpong_partner();
            (void)syscall1(SYS_exit_group, 0);
        }
    }
    if (partner < 0) { put("SWITCH fail spawn\n"); return; }

    char byte = 0;
    u64 started = now_ns();
    u64 completed = 0;
    for (u64 i = 0; i < rounds; i++) {
        if (syscall3(SYS_write, pingpong_up[1], (s64)&byte, 1) != 1) break;
        if (syscall3(SYS_read, pingpong_down[0], (s64)&byte, 1) != 1) break;
        completed++;
    }
    u64 elapsed = now_ns() - started;

    (void)syscall1(SYS_close, pingpong_up[1]);
    (void)syscall1(SYS_close, pingpong_down[0]);
    if (!threaded) (void)syscall4(SYS_wait4, partner, 0, 0, 0);

    put(threaded ? "SWITCH thread " : "SWITCH process ");
    put("roundtrips=");
    put_number(completed);
    put(" ns_each=");
    put_number(completed ? elapsed / completed : 0);
    put("\n");
    pin_to_all(cpus);
}

/* Whether the machine is using the processors it was given. */
/* Four children each doing a fixed amount of arithmetic, against one child
   doing it alone. */
/* The ratio is a number a fast context switch cannot fake, being 100 on one
   processor and on four whatever the scheduler managed to spread. */
static u64 time_workers(unsigned workers, u64 rounds_each) {
    s64 children[16];
    unsigned started = 0;
    u64 begun = now_ns();
    for (unsigned i = 0; i < workers && i < 16; i++) {
        s64 child = syscall1(SYS_fork, 0);
        if (child == 0) {
            spin_rounds(rounds_each);
            (void)syscall1(SYS_exit_group, 0);
        }
        if (child > 0) children[started++] = child;
    }
    for (unsigned i = 0; i < started; i++) (void)syscall4(SYS_wait4, children[i], 0, 0, 0);
    return now_ns() - begun;
}

static void test_parallel_speedup(unsigned workers, u64 rounds_each) {
    u64 alone = time_workers(1, rounds_each);
    u64 together = time_workers(workers, rounds_each);
    put("PARALLEL workers=");
    put_number(workers);
    put(" alone=");
    put_fixed(alone / 1000UL, 3);
    put("ms together=");
    put_fixed(together / 1000UL, 3);
    put("ms speedup=");
    put_fixed(together ? alone * (u64)workers * 100UL / together : 0, 2);
    put("\n");
}

/* How long a runnable task waits while an equal one has the processor, which is
   the quantum the scheduler handed out and nothing else. */
/* Both are pinned to one processor and neither ever sleeps, so every gap this
   measures is a full slice of the other's. */
/* The target latency is 6 ticks over however many are runnable, so two equals
   should each see about 3 ticks and four about 1 tick each between them. */
static void test_quantum(unsigned equals) {
    int channel[2];
    if (syscall1(SYS_pipe, (s64)channel) != 0) return;

    s64 others[16];
    unsigned started = 0;
    for (unsigned i = 0; i + 1 < equals && i < 16; i++) {
        s64 child = syscall1(SYS_fork, 0);
        if (child == 0) {
            pin_to_cpu(0);
            spin_forever();
        }
        if (child > 0) others[started++] = child;
    }

    s64 measurer = syscall1(SYS_fork, 0);
    if (measurer == 0) {
        (void)syscall1(SYS_close, channel[0]);
        pin_to_cpu(0);
        static u64 gaps[400];
        unsigned seen = 0;
        u64 last = now_ns();
        u64 deadline = last + 3000000000UL;
        while (seen < 400 && last < deadline) {
            for (unsigned i = 0; i < 8; i++) spin_round();
            u64 current = now_ns();
            u64 gap = current - last;
            /* Anything under a tick is this loop running, not waiting. */
            if (gap > 2000000UL) gaps[seen++] = gap;
            last = current;
        }
        sort_ascending(gaps, seen);
        u64 report[3];
        report[0] = seen;
        report[1] = seen ? gaps[seen / 2] : 0;
        report[2] = seen ? gaps[seen - 1] : 0;
        (void)syscall3(SYS_write, channel[1], (s64)report, sizeof(report));
        (void)syscall1(SYS_exit_group, 0);
    }
    (void)syscall1(SYS_close, channel[1]);

    u64 report[3] = {0, 0, 0};
    (void)syscall3(SYS_read, channel[0], (s64)report, sizeof(report));
    (void)syscall1(SYS_close, channel[0]);
    for (unsigned i = 0; i < started; i++) (void)syscall2(SYS_kill, others[i], SIGKILL);
    for (unsigned i = 0; i < started; i++) (void)syscall4(SYS_wait4, others[i], 0, 0, 0);
    (void)syscall4(SYS_wait4, measurer, 0, 0, 0);

    put("QUANTUM equals=");
    put_number(equals);
    put(" waits=");
    put_number(report[0]);
    put(" median=");
    put_fixed(report[1] / 1000UL, 3);
    put("ms max=");
    put_fixed(report[2] / 1000UL, 3);
    put("ms\n");
}

/* A thread that has been asleep for a while and then wants the processor. */
/* It sleeps for two seconds beside a spinner, wakes, and asks how long the
   spinner then went without running. */
/* A scheduler that hands a sleeper the credit for the whole time it slept
   answers with most of a second, where one that places a waking task next to
   the others answers with a quantum. */
static void test_sleeper_credit(void) {
    int channel[2];
    if (syscall1(SYS_pipe, (s64)channel) != 0) return;

    s64 spinner = syscall1(SYS_fork, 0);
    if (spinner == 0) {
        (void)syscall1(SYS_close, channel[0]);
        pin_to_cpu(0);
        /* The longest this ever went without running, over the window the
           sleeper is awake for. */
        u64 worst = 0;
        u64 last = now_ns();
        u64 deadline = last + 4000000000UL;
        while (last < deadline) {
            u64 accumulator = sink;
            for (unsigned i = 0; i < 512; i++) accumulator = accumulator * 6364136223846793005UL + 1442695040888963407UL;
            sink = accumulator;
            u64 current = now_ns();
            u64 gap = current - last;
            if (gap > worst) worst = gap;
            last = current;
        }
        (void)syscall3(SYS_write, channel[1], (s64)&worst, sizeof(worst));
        (void)syscall1(SYS_exit_group, 0);
    }
    (void)syscall1(SYS_close, channel[1]);

    /* Asleep for two of the spinner's four seconds, then awake and asking for
       the processor on the same one. */
    s64 sleeper = syscall1(SYS_fork, 0);
    if (sleeper == 0) {
        pin_to_cpu(0);
        sleep_ns(2000000000UL);
        (void)spin_for(1500000000UL);
        (void)syscall1(SYS_exit_group, 0);
    }

    u64 worst = 0;
    (void)syscall3(SYS_read, channel[0], (s64)&worst, sizeof(worst));
    (void)syscall1(SYS_close, channel[0]);
    (void)syscall4(SYS_wait4, spinner, 0, 0, 0);
    (void)syscall4(SYS_wait4, sleeper, 0, 0, 0);

    put("SLEEPER starved_spinner_for=");
    put_fixed(worst / 1000UL, 3);
    put("ms\n");
}

static int run_all(unsigned cpus) {
    put("BENCH START cpus=");
    put_number(cpus);
    put("\n");

    test_nice_ratio(4000000000UL);
    test_quantum(2);
    test_quantum(4);
    test_wake_latency(6, 200, cpus);
    test_switch_cost(1, 20000, cpus);
    test_switch_cost(0, 20000, cpus);
    test_parallel_speedup(4, 120000);
    test_sleeper_credit();

    put("BENCH DONE\n");
    return 0;
}

/* Init returning is a panic, and a panic is not the report anybody wants at the
   end of a run that worked. */
static void run_and_park(void) __attribute__((noreturn, used));
static void run_and_park(void) {
    (void)run_all(TUNIX_BENCH_CPUS);
    for (;;) sleep_ns(1000000000UL);
}

/* The entry point aligns the stack itself, because the ABI promises the
   compiler a 16-byte boundary that only a call from an aligned frame provides
   and there is no libc here to have done it already. */
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call run_and_park\n"
        "    hlt\n");
