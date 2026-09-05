/* What the kernel costs, measured from inside it, with no libc in between. */
/* It runs as init, prints one tagged line per measurement, and ends with PERF DONE. */

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_pipe 22
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_exit_group 231
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_clock_gettime 228
#define SYS_lseek 8
#define SYS_sched_setaffinity 203

#define CLOCK_MONOTONIC 1
#define SIGKILL 9
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define THREAD_FLAGS 0x10F00UL   /* VM | FS | FILES | SIGHAND | THREAD */

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

/* The child comes back on a stack of its own, which no C function can do. */
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

/* Everything printed goes to a file as well, so a machine with no serial
   cable can be read afterwards by mounting its disk. */
static int results_fd = -1;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
    if (results_fd >= 0) (void)syscall3(SYS_write, results_fd, (s64)text, (s64)length);
}

#define O_WRONLY_CREAT_TRUNC 0x241   /* O_WRONLY | O_CREAT | O_TRUNC */

static void open_results(void) {
    results_fd = (int)syscall3(SYS_open, (s64)"/tunix-perftest-results.txt",
                               O_WRONLY_CREAT_TRUNC, 0644);
}

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
    struct { s64 seconds, nanoseconds; } value = {0, 0};
    (void)syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.seconds * 1000000000UL + (u64)value.nanoseconds;
}

static void sleep_ns(u64 nanoseconds) {
    struct { s64 seconds, nanoseconds; } request;
    request.seconds = (s64)(nanoseconds / 1000000000UL);
    request.nanoseconds = (s64)(nanoseconds % 1000000000UL);
    (void)syscall2(SYS_nanosleep, (s64)&request, 0);
}

static void pin_to_cpu(unsigned cpu) {
    u64 mask = 1UL << cpu;
    (void)syscall3(SYS_sched_setaffinity, 0, sizeof(mask), (s64)&mask);
}

/* getpid does nothing but enter and leave, so what it times is the entry, the
   dispatch and everything the kernel does before it looks at the number. */
static u64 syscall_cost_ns(u64 count) {
    u64 begun = now_ns();
    for (u64 i = 0; i < count; i++) (void)syscall0(SYS_getpid);
    return (now_ns() - begun) / count;
}

/* The same cost with more processes on the queue, which is what shows whether
   anything on the entry path walks it. */
static void test_syscall_cost(void) {
    static s64 idle[400];
    static const unsigned steps[] = { 0, 64, 192 };
    int channel[2];
    if (syscall1(SYS_pipe, (s64)channel) != 0) { put("SYSCALL pipe failed\n"); return; }

    unsigned started = 0;
    for (unsigned step = 0; step < sizeof(steps) / sizeof(steps[0]); step++) {
        while (started < steps[step] && started < 400) {
            s64 child = syscall1(SYS_fork, 0);
            if (child == 0) {
                char byte;
                /* Asleep for the whole measurement, so they cost the queue its
                   length and nothing else. */
                (void)syscall3(SYS_read, channel[0], (s64)&byte, 1);
                (void)syscall1(SYS_exit_group, 0);
            }
            if (child <= 0) break;
            idle[started++] = child;
        }
        sleep_ns(200000000UL);
        put("SYSCALL processes=");
        put_number(started + 1);
        put(" getpid_ns=");
        put_number(syscall_cost_ns(200000));
        put("\n");
    }

    for (unsigned i = 0; i < started; i++) (void)syscall2(SYS_kill, idle[i], SIGKILL);
    /* WNOHANG and a bound, because a blocking wait4 here does not come back. */
    unsigned reaped = 0;
    for (unsigned round = 0; round < 200 && reaped < started; round++) {
        for (;;) {
            s64 got = syscall4(SYS_wait4, -1, 0, 1 /* WNOHANG */, 0);
            if (got <= 0) break;
            reaped++;
        }
        if (reaped < started) sleep_ns(20000000UL);
    }
    put("SYSCALL killed=");
    put_number(started);
    put(" reaped=");
    put_number(reaped);
    put("\n");
    (void)syscall1(SYS_close, channel[0]);
    (void)syscall1(SYS_close, channel[1]);
}

/* A byte through a pipe and back, which is two syscalls and a context switch. */
static void test_pipe_throughput(u64 bytes, u64 block_size) {
    int up[2], down[2];
    if (syscall1(SYS_pipe, (s64)up) != 0 || syscall1(SYS_pipe, (s64)down) != 0) return;
    static char block[4096];
    if (block_size > sizeof(block)) block_size = sizeof(block);

    s64 child = syscall1(SYS_fork, 0);
    if (child == 0) {
        (void)syscall1(SYS_close, up[1]);
        (void)syscall1(SYS_close, down[0]);
        for (;;) {
            s64 got = syscall3(SYS_read, up[0], (s64)block, (s64)block_size);
            if (got <= 0) break;
            if (syscall3(SYS_write, down[1], (s64)block, 1) != 1) break;
        }
        (void)syscall1(SYS_exit_group, 0);
    }
    (void)syscall1(SYS_close, up[0]);
    (void)syscall1(SYS_close, down[1]);

    u64 sent = 0;
    u64 begun = now_ns();
    while (sent < bytes) {
        if (syscall3(SYS_write, up[1], (s64)block, (s64)block_size) != (s64)block_size) break;
        char reply;
        if (syscall3(SYS_read, down[0], (s64)&reply, 1) != 1) break;
        sent += block_size;
    }
    u64 elapsed = now_ns() - begun;
    (void)syscall1(SYS_close, up[1]);
    (void)syscall1(SYS_close, down[0]);
    (void)syscall4(SYS_wait4, child, 0, 0, 0);

    put("PIPE block=");
    put_number(block_size);
    put(" MB_per_s=");
    put_number(elapsed ? sent * 1000UL / elapsed : 0);
    put(" roundtrip_ns=");
    put_number(sent ? elapsed / (sent / block_size) : 0);
    put("\n");
}

/* The first touch of an anonymous page, which is a fault and a page committed. */
static void test_page_fault(u64 pages) {
    u64 length = pages * 4096UL;
    s64 base = syscall6(SYS_mmap, 0, (s64)length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((u64)base >= (u64)-4095L) { put("FAULT mmap failed\n"); return; }
    u64 begun = now_ns();
    for (u64 i = 0; i < pages; i++) *(volatile char *)(base + i * 4096UL) = 1;
    u64 elapsed = now_ns() - begun;
    (void)syscall2(SYS_munmap, base, (s64)length);
    put("FAULT pages=");
    put_number(pages);
    put(" ns_each=");
    put_number(elapsed / pages);
    put("\n");
}

/* fork, and the child leaving straight away, so what it times is the copy of an
   address space and the teardown of one. */
static void test_fork_cost(u64 count, u64 extra_pages) {
    s64 extra = 0;
    if (extra_pages) {
        extra = syscall6(SYS_mmap, 0, (s64)(extra_pages * 4096UL), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((u64)extra >= (u64)-4095L) extra = 0;
        else for (u64 i = 0; i < extra_pages; i++)
            *(volatile char *)(extra + i * 4096UL) = 1;
    }
    u64 begun = now_ns();
    u64 done = 0;
    for (u64 i = 0; i < count; i++) {
        s64 child = syscall1(SYS_fork, 0);
        if (child == 0) (void)syscall1(SYS_exit_group, 0);
        if (child < 0) break;
        (void)syscall4(SYS_wait4, child, 0, 0, 0);
        done++;
    }
    u64 elapsed = now_ns() - begun;
    if (extra) (void)syscall2(SYS_munmap, extra, (s64)(extra_pages * 4096UL));
    put("FORK mapped_pages=");
    put_number(extra_pages);
    put(" us_each=");
    put_fixed(done ? elapsed / done : 0, 3);
    put("\n");
}

/* Reading a file the kernel already has cached, which is the copy and the VFS
   walk and nothing else. */
static void test_file_read(u64 rounds) {
    int fd = (int)syscall3(SYS_open, (s64)"/sbin/init", 0, 0);
    if (fd < 0) { put("FILE open failed\n"); return; }
    static char block[4096];
    u64 total = 0;
    u64 reads = 0;
    u64 begun = now_ns();
    for (u64 i = 0; i < rounds; i++) {
        s64 got = syscall3(SYS_read, fd, (s64)block, sizeof(block));
        if (got <= 0) {
            (void)syscall3(SYS_lseek, fd, 0, 0);
            continue;
        }
        total += (u64)got;
        reads++;
    }
    u64 elapsed = now_ns() - begun;
    (void)syscall1(SYS_close, fd);
    put("FILE bytes=");
    put_number(total);
    put(" MB_per_s=");
    put_number(elapsed ? total * 1000UL / elapsed : 0);
    put(" read_ns=");
    put_number(reads ? elapsed / reads : 0);
    put("\n");
}

/* Creating a thread shares the address space and the descriptor table, so what
   it costs is everything fork does except cloning those two. */
static volatile u64 thread_done;
static void thread_body(void) { thread_done = 1; }

/* fork and the child leaving, with nothing waited for until the end, so what it
   times is creation and teardown without the wait. */
static void test_fork_nowait(u64 count) {
    u64 begun = now_ns();
    u64 done = 0;
    for (u64 i = 0; i < count; i++) {
        s64 child = syscall1(SYS_fork, 0);
        if (child == 0) (void)syscall1(SYS_exit_group, 0);
        if (child < 0) break;
        done++;
    }
    u64 elapsed = now_ns() - begun;
    for (u64 i = 0; i < done; i++) (void)syscall4(SYS_wait4, -1, 0, 0, 0);
    put("FORKNOWAIT count=");
    put_number(done);
    put(" us_each=");
    put_fixed(done ? elapsed / done : 0, 3);
    put("\n");
}

static void test_thread_cost(u64 count) {
    static char stack[65536] __attribute__((aligned(16)));
    u64 begun = now_ns();
    u64 done = 0;
    for (u64 i = 0; i < count; i++) {
        thread_done = 0;
        char *top = stack + sizeof(stack) - 8;
        *(void **)top = (void *)thread_body;
        if (spawn_thread(THREAD_FLAGS, top) < 0) break;
        while (!thread_done) { }
        done++;
    }
    u64 elapsed = now_ns() - begun;
    put("THREAD count=");
    put_number(done);
    put(" us_each=");
    put_fixed(done ? elapsed / done : 0, 3);
    put("\n");
}

/* Unmapping while other threads of the same process are running.
   Each page used to interrupt every processor sharing the address space and
   spin until all of them had answered, so the cost was per page and only
   appeared on a machine with more than one processor. */
static volatile int helpers_stop;
static volatile unsigned helpers_running;

static void helper_body(void) {
    __atomic_add_fetch(&helpers_running, 1, __ATOMIC_RELAXED);
    while (!__atomic_load_n(&helpers_stop, __ATOMIC_RELAXED)) { }
}

#define MAX_HELPERS 3

static void test_unmap_shootdown(u64 pages, unsigned helpers) {
    static char stacks[MAX_HELPERS][65536] __attribute__((aligned(16)));
    if (helpers > MAX_HELPERS) helpers = MAX_HELPERS;
    __atomic_store_n(&helpers_stop, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&helpers_running, 0, __ATOMIC_RELAXED);

    unsigned started = 0;
    for (unsigned index = 0; index < helpers; index++) {
        char *top = stacks[index] + sizeof(stacks[index]) - 8;
        *(void **)top = (void *)helper_body;
        if (spawn_thread(THREAD_FLAGS, top) < 0) break;
        started++;
    }
    /* They have to be on a processor, not merely created: a thread that has not
       run yet is not looking at the address space. */
    while (__atomic_load_n(&helpers_running, __ATOMIC_RELAXED) < started) { }

    u64 length = pages * 4096UL;
    s64 base = syscall6(SYS_mmap, 0, (s64)length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((u64)base >= (u64)-4095L) {
        __atomic_store_n(&helpers_stop, 1, __ATOMIC_RELAXED);
        put("SHOOTDOWN mmap failed\n");
        return;
    }
    for (u64 i = 0; i < pages; i++) *(volatile char *)(base + i * 4096UL) = 1;

    u64 begun = now_ns();
    (void)syscall2(SYS_munmap, base, (s64)length);
    u64 elapsed = now_ns() - begun;

    __atomic_store_n(&helpers_stop, 1, __ATOMIC_RELAXED);
    sleep_ns(50000000UL);

    put("SHOOTDOWN threads=");
    put_number(started);
    put(" pages=");
    put_number(pages);
    put(" ns_each=");
    put_number(elapsed / pages);
    put(" total_us=");
    put_fixed(elapsed / 1000UL, 0);
    put("\n");
}

/* One number out of a /proc file that holds `name value` lines. */
static u64 proc_value(const char *path, const char *name) {
    char text[512];
    int fd = (int)syscall3(SYS_open, (s64)path, 0, 0);
    if (fd < 0) return 0;
    s64 got = syscall3(SYS_read, fd, (s64)text, sizeof(text) - 1);
    (void)syscall1(SYS_close, fd);
    if (got <= 0) return 0;
    text[got] = 0;
    for (s64 i = 0; i < got; i++) {
        u64 k = 0;
        while (name[k] && i + (s64)k < got && text[i + k] == name[k]) k++;
        if (name[k] || text[i + k] != ' ') continue;
        u64 value = 0;
        for (s64 j = i + (s64)k + 1; j < got && text[j] >= '0' && text[j] <= '9'; j++)
            value = value * 10 + (u64)(text[j] - '0');
        return value;
    }
    return 0;
}

/* What a startup costs the disk. */
/* Opening several hundred files is what runit and a compositor do, and every
   inode, directory block and indirect block behind them is a read that reaches
   the medium with the kernel lock held. */
static void test_startup_reads(unsigned count) {
    u64 reads_before = proc_value("/proc/blockstat", "reads");
    u64 sectors_before = proc_value("/proc/blockstat", "sectors");
    u64 wait_before = proc_value("/proc/blockstat", "wait_ns");

    static char block[4096];
    u64 opened = 0, bytes = 0;
    u64 begun = now_ns();
    for (unsigned index = 0; index < count; index++) {
        char path[32];
        const char *prefix = "/files/f";
        u64 at = 0;
        while (prefix[at]) { path[at] = prefix[at]; at++; }
        path[at++] = (char)('0' + (index / 100) % 10);
        path[at++] = (char)('0' + (index / 10) % 10);
        path[at++] = (char)('0' + index % 10);
        path[at] = 0;
        int fd = (int)syscall3(SYS_open, (s64)path, 0, 0);
        if (fd < 0) continue;
        opened++;
        for (;;) {
            s64 got = syscall3(SYS_read, fd, (s64)block, sizeof(block));
            if (got <= 0) break;
            bytes += (u64)got;
        }
        (void)syscall1(SYS_close, fd);
    }
    u64 elapsed = now_ns() - begun;

    put("STARTUP files=");
    put_number(opened);
    put(" bytes=");
    put_number(bytes);
    put(" ms=");
    put_fixed(elapsed / 1000UL, 3);
    put(" reads=");
    put_number(proc_value("/proc/blockstat", "reads") - reads_before);
    put(" sectors=");
    put_number(proc_value("/proc/blockstat", "sectors") - sectors_before);
    put(" disk_ms=");
    put_fixed((proc_value("/proc/blockstat", "wait_ns") - wait_before) / 1000UL, 3);
    put("\n");
}

static int run_all(void) {
    open_results();
    put("PERF START\n");
    pin_to_cpu(0);
    /* The queue-length test goes last: it leaves processes behind, and every
       syscall the others make would then be paying for them. */
    /* Before anything maps memory, so the address space it clones is only what
       the program started with. */
    test_fork_cost(300, 0);
    test_thread_cost(300);
    test_pipe_throughput(4UL * 1024 * 1024, 64);
    test_pipe_throughput(64UL * 1024 * 1024, 4096);
    test_page_fault(20000);
    test_fork_cost(300, 0);
    test_fork_nowait(300);
    test_fork_cost(200, 16384);
    test_unmap_shootdown(8192, 0);
    test_unmap_shootdown(8192, 3);
    test_file_read(20000);
    /* Late, because reading a few megabytes leaves the heap in a state the
       others would then be measuring: fork takes a 32 KiB kernel stack from it
       and went from 17.9 to 50 us when this ran first. */
    test_startup_reads(400);
    test_syscall_cost();
    put("PERF DONE\n");
    return 0;
}

/* Init returning is a panic, which is not the report anybody wants. */
static void run_and_park(void) __attribute__((noreturn, used));
static void run_and_park(void) {
    (void)run_all();
    for (;;) sleep_ns(1000000000UL);
}

/* The entry point aligns the stack itself, because there is no libc here to
   have done it and the compiler is promised a 16-byte boundary. */
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call run_and_park\n"
        "    hlt\n");
