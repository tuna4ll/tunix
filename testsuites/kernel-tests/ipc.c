#define TEST_NAME "ipc"
#include "test.h"

#if defined(__x86_64__)
#define SYS_semget     64
#define SYS_semop      65
#define SYS_semctl     66
#define SYS_semtimedop 220
#else
#define SYS_semget     190
#define SYS_semctl     191
#define SYS_semtimedop 192
#define SYS_semop      193
#endif

#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_RMID    0
#define IPC_STAT    2
#define GETPID      11
#define GETVAL      12
#define GETALL      13
#define GETNCNT     14
#define SETVAL      16
#define SETALL      17
#define SEM_UNDO    0x1000

#define EINTR  4
#define EINVAL 22
#define EIDRM  43
#define ERANGE 34

struct sembuf {
    u16 num;
    short op;
    short flags;
};

struct semid_ds {
    int key;
    u32 uid;
    u32 gid;
    u32 cuid;
    u32 cgid;
    u32 mode;
    u16 seq;
    u16 pad;
    u32 pad2;
    u64 unused1;
    u64 unused2;
    s64 otime;
#if defined(__x86_64__)
    u64 unused3;
#endif
    s64 ctime;
#if defined(__x86_64__)
    u64 unused4;
#endif
    u64 nsems;
    u64 unused5;
    u64 unused6;
};

static s64 semget(int key, int count, int flags) {
    return SYSCALL(SYS_semget, key, count, flags, 0, 0, 0);
}

static s64 semop1(int id, u16 num, short op, short flags) {
    struct sembuf operation = {num, op, flags};
    return SYSCALL(SYS_semop, id, &operation, 1, 0, 0, 0);
}

static s64 semctl(int id, int num, int command, u64 argument) {
    return SYSCALL(SYS_semctl, id, num, command, argument, 0, 0);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s64 id = semget(IPC_PRIVATE, 2, 0600);
    expect(id >= 0, "a private set of two semaphores is created");
    expect_eq(semctl((int)id, 0, GETVAL, 0), 0, "semaphores start at zero");
    expect_eq(semctl((int)id, 0, SETVAL, 1), 0, "SETVAL works");
    expect_eq(semctl((int)id, 0, GETVAL, 0), 1, "and GETVAL sees it");
    expect_eq(semop1((int)id, 0, -1, 0), 0, "a decrement that fits succeeds at once");
    expect_eq(semop1((int)id, 0, -1, IPC_NOWAIT), -EAGAIN, "one that does not fit refuses to wait");
    expect_eq(semctl((int)id, 0, GETPID, 0), getpid(), "the last operating process is recorded");

    s64 child = fork();
    if (child == 0) exit(semop1((int)id, 0, -1, 0) == 0 ? 0 : 1);
    sleep_ms(150);
    expect_eq(semctl((int)id, 0, GETNCNT, 0), 1, "a blocked decrement is counted");
    expect_eq(semop1((int)id, 0, 1, 0), 0, "an increment is accepted");
    int status = -1;
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "and wakes the waiting process");
    expect_eq(semctl((int)id, 0, GETVAL, 0), 0, "which takes the unit");

    child = fork();
    if (child == 0) {
        semop1((int)id, 1, 3, SEM_UNDO);
        exit(0);
    }
    waitpid(child, &status, 0);
    expect_eq(semctl((int)id, 1, GETVAL, 0), 0, "SEM_UNDO is reversed when the process exits");

    struct sembuf pair[2] = {{0, 1, 0}, {1, -1, IPC_NOWAIT}};
    expect_eq(SYSCALL(SYS_semop, id, pair, 2, 0, 0, 0), -EAGAIN, "operations are all or nothing");
    expect_eq(semctl((int)id, 0, GETVAL, 0), 0, "so the first one did not happen");

    struct timespec limit = {0, 100000000};
    s64 start = now_ms();
    struct sembuf wait_one = {0, -1, 0};
    expect_eq(SYSCALL(SYS_semtimedop, id, &wait_one, 1, &limit, 0, 0), -EAGAIN,
              "semtimedop gives up");
    s64 waited = now_ms() - start;
    expect(waited >= 90 && waited < 1000, "after its timeout");

    u16 values[2] = {5, 7};
    expect_eq(semctl((int)id, 0, SETALL, (u64)values), 0, "SETALL sets every value");
    u16 back[2] = {0, 0};
    expect_eq(semctl((int)id, 0, GETALL, (u64)back), 0, "GETALL reads them");
    expect(back[0] == 5 && back[1] == 7, "in order");
    expect_eq(semctl((int)id, 0, SETVAL, 40000), -ERANGE, "values above the maximum are refused");

    struct semid_ds info;
    expect_eq(semctl((int)id, 0, IPC_STAT, (u64)&info), 0, "IPC_STAT answers");
    expect(info.nsems == 2 && (info.mode & 0777) == 0600, "with the size and the mode");

    child = fork();
    if (child == 0) exit(semop1((int)id, 1, -100, 0) == -EIDRM ? 0 : 1);
    sleep_ms(150);
    expect_eq(semctl((int)id, 0, IPC_RMID, 0), 0, "the set is removed");
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "a waiter learns that it was removed");
    expect_eq(semop1((int)id, 0, 1, 0), -EINVAL, "and the identifier is dead");

    s64 keyed = semget(0x7475, 1, IPC_CREAT | 0600);
    expect(keyed >= 0, "a keyed set is created");
    expect_eq(semget(0x7475, 1, 0600), keyed, "the key finds it again");
    expect_eq(semget(0x7475, 1, IPC_CREAT | IPC_EXCL | 0600), -EEXIST, "IPC_EXCL refuses it");
    expect_eq(semget(0x7475, 2, 0600), -EINVAL, "a larger set than exists is refused");
    expect_eq(semget(0x7476, 1, 0600), -ENOENT, "an unknown key without IPC_CREAT is refused");
    expect_eq(semctl((int)keyed, 0, IPC_RMID, 0), 0, "the keyed set is removed");
}
