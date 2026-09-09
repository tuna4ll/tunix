typedef unsigned long u64;
typedef long s64;

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_RT_SIGACTION 13
#define SYS_WAIT4 61
#define SYS_EXIT_GROUP 231
#define SYS_CLONE3 435

#define O_RDONLY 0
#define O_WRONLY 1
#define SIGCHLD 17
#define SIGUSR1 10
#define E2BIG 7
#define EINVAL 22
#define CLONE_VM 0x00000100UL
#define CLONE_FS 0x00000200UL
#define CLONE_FILES 0x00000400UL
#define CLONE_SIGHAND 0x00000800UL
#define CLONE_VFORK 0x00004000UL
#define CLONE_THREAD 0x00010000UL
#define CLONE_SYSVSEM 0x00040000UL
#define CLONE_PARENT_SETTID 0x00100000UL
#define CLONE_CHILD_CLEARTID 0x00200000UL
#define CLONE_CLEAR_SIGHAND (1UL << 32)

struct clone_args {
    u64 flags;
    u64 pidfd;
    u64 child_tid;
    u64 parent_tid;
    u64 exit_signal;
    u64 stack;
    u64 stack_size;
    u64 tls;
    u64 set_tid;
    u64 set_tid_size;
    u64 cgroup;
};

struct sigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

static unsigned char child_stack[65536] __attribute__((aligned(16)));
static unsigned char thread_stack[65536] __attribute__((aligned(16)));
static volatile u64 thread_done;
static volatile unsigned thread_tid;

extern s64 clone3_vfork(struct clone_args *args, u64 size);
extern s64 clone3_thread(struct clone_args *args, u64 size);

__asm__(
    ".text\n"
    ".global clone3_vfork\n"
    "clone3_vfork:\n"
    "mov $435, %rax\n"
    "syscall\n"
    "test %rax, %rax\n"
    "jz 1f\n"
    "ret\n"
    "1:\n"
    "xor %edi, %edi\n"
    "mov $231, %rax\n"
    "syscall\n"
    "ud2\n");

__asm__(
    ".text\n"
    ".global clone3_thread\n"
    "clone3_thread:\n"
    "mov $435, %rax\n"
    "syscall\n"
    "test %rax, %rax\n"
    "jz 1f\n"
    "ret\n"
    "1:\n"
    "movq $1, thread_done(%rip)\n"
    "xor %edi, %edi\n"
    "mov $60, %rax\n"
    "syscall\n"
    "ud2\n");

static inline s64 call1(s64 number, s64 first) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first)
                     : "rcx", "r11", "memory");
    return result;
}

static inline s64 call2(s64 number, s64 first, s64 second) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first),
                     "S"(second) : "rcx", "r11", "memory");
    return result;
}

static inline s64 call3(s64 number, s64 first, s64 second, s64 third) {
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first),
                     "S"(second), "d"(third) : "rcx", "r11", "memory");
    return result;
}

static inline s64 call4(s64 number, s64 first, s64 second, s64 third,
                        s64 fourth) {
    register s64 r10 __asm__("r10") = fourth;
    s64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first),
                     "S"(second), "d"(third), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static u64 text_length(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    return length;
}

static void print(const char *text) {
    (void)call3(SYS_WRITE, 1, (s64)text, (s64)text_length(text));
}

static int contains(const char *data, s64 length, const char *needle) {
    u64 wanted = text_length(needle);
    if (length < 0 || (u64)length < wanted) return 0;
    for (u64 at = 0; at + wanted <= (u64)length; at++) {
        u64 index = 0;
        while (index < wanted && data[at + index] == needle[index]) index++;
        if (index == wanted) return 1;
    }
    return 0;
}

static void zero(void *data, u64 size) {
    unsigned char *bytes = data;
    for (u64 index = 0; index < size; index++) bytes[index] = 0;
}

static int clear_gaps(void) {
    s64 fd = call3(SYS_OPEN, (s64)"/proc/abi_gaps", O_WRONLY, 0);
    if (fd < 0) return 0;
    s64 result = call3(SYS_WRITE, fd, (s64)"0", 1);
    (void)call1(SYS_CLOSE, fd);
    return result == 1;
}

static int gaps_have_clone3(void) {
    char output[1024];
    s64 fd = call3(SYS_OPEN, (s64)"/proc/abi_gaps", O_RDONLY, 0);
    if (fd < 0) return 1;
    s64 length = call3(SYS_READ, fd, (s64)output, sizeof(output));
    (void)call1(SYS_CLOSE, fd);
    return contains(output, length, "syscall 435 ");
}

static int run_test(void) {
    if (!clear_gaps()) return 0;

    struct sigaction action;
    zero(&action, sizeof(action));
    action.handler = 0x1234;
    if (call4(SYS_RT_SIGACTION, SIGUSR1, (s64)&action, 0, 8) != 0) return 0;

    struct clone_args args;
    zero(&args, sizeof(args));
    args.flags = CLONE_CLEAR_SIGHAND;
    args.exit_signal = SIGCHLD;
    s64 child = call2(SYS_CLONE3, (s64)&args, sizeof(args));
    if (child == 0) {
        struct sigaction inherited;
        zero(&inherited, sizeof(inherited));
        s64 result = call4(SYS_RT_SIGACTION, SIGUSR1, 0,
                           (s64)&inherited, 8);
        call1(SYS_EXIT_GROUP, result == 0 && inherited.handler == 0 ? 0 : 1);
        for (;;) { }
    }
    if (child < 0) return 0;

    int status = -1;
    if (call4(SYS_WAIT4, child, (s64)&status, 0, 0) != child || status != 0)
        return 0;

    struct sigaction parent_action;
    zero(&parent_action, sizeof(parent_action));
    if (call4(SYS_RT_SIGACTION, SIGUSR1, 0, (s64)&parent_action, 8) != 0 ||
        parent_action.handler != action.handler)
        return 0;

    zero(&args, sizeof(args));
    args.flags = CLONE_VM | CLONE_VFORK | CLONE_CLEAR_SIGHAND;
    args.exit_signal = SIGCHLD;
    args.stack = (u64)child_stack;
    args.stack_size = sizeof(child_stack);
    child = clone3_vfork(&args, sizeof(args));
    status = -1;
    if (child < 0 || call4(SYS_WAIT4, child, (s64)&status, 0, 0) != child ||
        status != 0)
        return 0;

    zero(&args, sizeof(args));
    thread_done = 0;
    thread_tid = 0;
    args.flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID |
                 CLONE_CHILD_CLEARTID;
    args.pidfd = (u64)&thread_tid;
    args.child_tid = (u64)&thread_tid;
    args.parent_tid = (u64)&thread_tid;
    args.stack = (u64)thread_stack;
    args.stack_size = sizeof(thread_stack);
    child = clone3_thread(&args, sizeof(args));
    if (child < 0 || thread_done != 1 || thread_tid != 0) return 0;

    args.flags = CLONE_CLEAR_SIGHAND | CLONE_SIGHAND;
    if (call2(SYS_CLONE3, (s64)&args, sizeof(args)) != -EINVAL) return 0;
    args.flags = 0;
    if (call2(SYS_CLONE3, (s64)&args, 63) != -EINVAL) return 0;
    if (call2(SYS_CLONE3, (s64)&args, sizeof(args) + 1) != -E2BIG) return 0;
    return !gaps_have_clone3();
}

void _start(void) {
    if (run_test()) print("CLONE3TEST PASS\n");
    else print("CLONE3TEST FAIL\n");
    print("CLONE3TEST DONE\n");
    call1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}
