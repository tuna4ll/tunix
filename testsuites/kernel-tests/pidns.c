#define TEST_NAME "pidns"
#include "test.h"

#if defined(__x86_64__)
#define SYS_clone_nr   56
#define SYS_getppid_nr 110
#else
#define SYS_clone_nr   220
#define SYS_getppid_nr 173
#endif

#define CLONE_NEWPID 0x20000000UL
#define SIGCHLD_V    17
#define ESRCH        3

static s64 clone_newpid(void) {
    return SYSCALL(SYS_clone_nr, CLONE_NEWPID | SIGCHLD_V, 0, 0, 0, 0, 0);
}
static s64 my_ppid(void) { return SYSCALL(SYS_getppid_nr, 0, 0, 0, 0, 0, 0); }

static s64 init_is_pid1(void) {
    s64 pid = clone_newpid();
    if (pid == 0) {
        s64 me = getpid();
        s64 gp = fork();
        if (gp == 0) exit((int)getpid());
        int st = 0;
        waitpid(gp, &st, 0);
        s64 gchild = exited_with(st);
        exit(me == 1 && gchild == 2 ? 0 : 50);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return exited_with(status);
}

static s64 ppid_zero_for_init(void) {
    s64 pid = clone_newpid();
    if (pid == 0) {
        s64 pp = my_ppid();
        s64 gp = fork();
        if (gp == 0) exit((int)my_ppid());
        int st = 0;
        waitpid(gp, &st, 0);
        s64 gchild_pp = exited_with(st);
        exit(pp == 0 && gchild_pp == 1 ? 0 : 51);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return exited_with(status);
}

static s64 isolated_from_outside(void) {
    s64 pid = clone_newpid();
    if (pid == 0) {
        s64 self = kill(1, 0);
        s64 ghost = kill(9999, 0);
        exit(self == 0 && ghost == -ESRCH ? 0 : 52);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return exited_with(status);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    expect_eq(init_is_pid1(), 0, "clone(NEWPID) makes the child pid 1 and its child pid 2");
    expect_eq(ppid_zero_for_init(), 0, "the namespace init sees ppid 0, its child sees ppid 1");
    expect_eq(isolated_from_outside(), 0, "a pid namespace cannot signal a pid outside it");
}
