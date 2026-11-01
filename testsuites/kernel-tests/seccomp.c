#define TEST_NAME "seccomp"
#include "test.h"

#if defined(__x86_64__)
#define SYS_prctl_nr   157
#define SYS_seccomp_nr 317
#define NR_getpid      39
#define NR_getppid     110
#define NR_gettid      186
#define NR_write       1
#define NR_exit_group  231
#define AUDIT_ARCH     0xC000003EU
#else
#define SYS_prctl_nr   167
#define SYS_seccomp_nr 277
#define NR_getpid      172
#define NR_getppid     173
#define NR_gettid      178
#define NR_write       64
#define NR_exit_group  94
#define AUDIT_ARCH     0xC00000B7U
#endif

#define PR_SET_NO_NEW_PRIVS     38
#define PR_SET_SECCOMP          22
#define SECCOMP_MODE_STRICT     1
#define SECCOMP_MODE_FILTER     2
#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1

#define BPF_LD                   0x00
#define BPF_W                    0x00
#define BPF_ABS                  0x20
#define BPF_JMP                  0x05
#define BPF_JEQ                  0x10
#define BPF_K                    0x00
#define BPF_RET                  0x06
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_ALLOW        0x7fff0000U
#define EPERM                    1
#define EINVAL                   22
#define ENOSYS                   38

struct sock_filter {
    u16 code;
    u8 jt;
    u8 jf;
    u32 k;
};
struct sock_fprog {
    u16 len;
    u64 filter;
};

#define STMT(code, k)       {(code), 0, 0, (k)}
#define JUMP(code, k, t, f) {(code), (t), (f), (k)}

static s64 set_filter(struct sock_filter *program, u16 length, u32 flags) {
    struct sock_fprog prog = {length, (u64)program};
    return SYSCALL(SYS_seccomp_nr, SECCOMP_SET_MODE_FILTER, flags, (s64)&prog, 0, 0, 0);
}

static void no_new_privs(void) { SYSCALL(SYS_prctl_nr, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0); }

static int child_status(s64 (*body)(void)) {
    s64 pid = fork();
    if (pid == 0) exit((int)body());
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

static s64 strict_ok(void) {
    no_new_privs();
    if (SYSCALL(SYS_prctl_nr, 22, SECCOMP_MODE_STRICT, 0, 0, 0, 0) != 0) return 90;
    write(1, "", 0);
    return 0;
}

static s64 strict_kill(void) {
    no_new_privs();
    SYSCALL(SYS_prctl_nr, 22, SECCOMP_MODE_STRICT, 0, 0, 0, 0);
    getppid();
    return 0;
}

static s64 filter_errno(void) {
    no_new_privs();
    static struct sock_filter program[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, 4),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH, 1, 0),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_getppid, 0, 1),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    if (set_filter(program, 7, 0) != 0) return 91;
    if (getpid() <= 0) return 92;
    if (getppid() != -EPERM) return 93;
    return 0;
}

static s64 filter_kill(void) {
    no_new_privs();
    static struct sock_filter program[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_gettid, 0, 1),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    if (set_filter(program, 4, 0) != 0) return 94;
    getpid();
    SYSCALL(NR_gettid, 0, 0, 0, 0, 0, 0);
    return 0;
}

static volatile int trap_seen;
static void sigsys_handler(int signal) {
    (void)signal;
    trap_seen = 1;
}

static s64 filter_trap(void) {
    no_new_privs();
    signal(31, (u64)sigsys_handler);
    static struct sock_filter program[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_getppid, 0, 1),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    if (set_filter(program, 4, 0) != 0) return 95;
    getppid();
    return trap_seen ? 0 : 96;
}

static s64 root_no_nnp(void) {
    static struct sock_filter program[] = {
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    return set_filter(program, 1, 0);
}

static s64 bad_program(void) {
    no_new_privs();
    static struct sock_filter program[] = {
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 50, 0),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    return set_filter(program, 2, 0);
}

static s64 inherited(void) {
    no_new_privs();
    static struct sock_filter program[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_getppid, 0, 1),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    if (set_filter(program, 4, 0) != 0) return 97;
    s64 pid = fork();
    if (pid == 0) exit(getppid() == -EPERM ? 0 : 98);
    int status = 0;
    waitpid(pid, &status, 0);
    return exited_with(status);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    expect_eq(exited_with(child_status(strict_ok)), 0, "strict mode allows read/write/exit");
    expect_eq(killed_by(child_status(strict_kill)), 31, "strict mode kills a forbidden call");
    expect_eq(exited_with(child_status(filter_errno)), 0, "a filter can return EPERM for one call");
    expect_eq(killed_by(child_status(filter_kill)), 31, "a filter can kill the process");
    expect_eq(exited_with(child_status(filter_trap)), 0, "a filter can trap to a SIGSYS handler");
    expect_eq(exited_with(child_status(root_no_nnp)), 0,
              "root installs a filter without no_new_privs");
    expect_eq(exited_with(child_status(bad_program)), (int)(u8)(u32)(-EINVAL),
              "an out-of-range jump is rejected");
    expect_eq(exited_with(child_status(inherited)), 0, "a fork inherits the filter");
}
