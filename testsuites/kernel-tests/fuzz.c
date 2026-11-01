#define TEST_NAME "fuzz"
#include "test.h"

#if defined(__x86_64__)
#define NR_MAX             335
#define SYS_mmap_nr        9
#define SYS_openat_nr      257
#define SYS_pipe2_nr       293
#define SYS_socket_nr      41
#define SYS_eventfd_nr     290
#define SYS_memfd_nr       319
#define SYS_epollcreate_nr 291
#define SYS_getpid_nr      39
#define SYS_setitimer_nr   38
#define SYS_alarm_nr       37
#define SYS_block_a        38
#define SYS_block_b        37
#else
#define NR_MAX             290
#define SYS_mmap_nr        222
#define SYS_openat_nr      56
#define SYS_pipe2_nr       59
#define SYS_socket_nr      198
#define SYS_eventfd_nr     19
#define SYS_memfd_nr       279
#define SYS_epollcreate_nr 20
#define SYS_getpid_nr      172
#define SYS_setitimer_nr   103
#define SYS_alarm_nr       -1
#define SYS_block_a        103
#define SYS_block_b        102
#endif

static u64 rng_state;
static u64 rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static int blocked(s64 nr) {
    switch (nr) {
#if defined(__x86_64__)
    case 231:
    case 60:
    case 59:
    case 169:
    case 101:
    case 322:
    case 11:
    case 10:
    case 25:
    case 56:
    case 57:
    case 58:
    case 15:
    case 272:
    case 12:
    case 38:
    case 37:
    case 36:
    case 35:
#else
    case 94:
    case 93:
    case 221:
    case 142:
    case 117:
    case 260:
    case 215:
    case 226:
    case 216:
    case 220:
    case 139:
    case 214:
    case 103:
    case 102:
    case 101:
    case 107:
    case 109:
#endif
        return 1;
    default: return 0;
    }
}

static int fds[32];
static int fd_count;
static u8 scratch[65536];

static void seed_fds(void) {
    fd_count = 0;
    int pair[2];
    if (SYSCALL(SYS_pipe2_nr, pair, 0, 0, 0, 0, 0) == 0) {
        fds[fd_count++] = pair[0];
        fds[fd_count++] = pair[1];
    }
    s64 f = SYSCALL(SYS_openat_nr, -100, "/proc/self/status", 0, 0, 0, 0);
    if (f >= 0) fds[fd_count++] = (int)f;
    f = SYSCALL(SYS_openat_nr, -100, "/tmp/f", 0100 | 02, 0666, 0, 0);
    if (f >= 0) fds[fd_count++] = (int)f;
    f = SYSCALL(SYS_memfd_nr, "m", 0, 0, 0, 0, 0);
    if (f >= 0) fds[fd_count++] = (int)f;
    f = SYSCALL(SYS_eventfd_nr, 0, 0, 0, 0, 0, 0);
    if (f >= 0) fds[fd_count++] = (int)f;
    f = SYSCALL(SYS_epollcreate_nr, 0, 0, 0, 0, 0, 0);
    if (f >= 0) fds[fd_count++] = (int)f;
    f = SYSCALL(SYS_socket_nr, 1, 1, 0, 0, 0, 0);
    if (f >= 0) fds[fd_count++] = (int)f;
    f = SYSCALL(SYS_socket_nr, 2, 2, 0, 0, 0, 0);
    if (f >= 0) fds[fd_count++] = (int)f;
}

static s64 pick_arg(u64 r) {
    switch (r & 7) {
    case 0:  return fd_count ? fds[(r >> 3) % (u64)fd_count] : 0;
    case 1:  return (s64)(r >> 3) & 0xFFF;
    case 2:  return (s64)scratch;
    case 3:  return (s64)(scratch + ((r >> 3) & 0x3FFF));
    case 4:  return 0;
    case 5:  return (s64) "/tmp/f";
    case 6:  return -1;
    default: return (s64)(r >> 3);
    }
}

static void worker(u64 seed, int iterations) {
    rng_state = seed ? seed : 1;
    for (int sig = 1; sig <= 31; sig++) signal(sig, 0);
    struct itimerspec deadline = {{0, 0}, {3, 0}};
    SYSCALL(SYS_setitimer_nr, 0, &deadline, 0, 0, 0, 0);
    close(0);
    close(1);
    close(2);
    seed_fds();
    for (int i = 0; i < iterations; i++) {
        s64 nr = (s64)(rng() % (NR_MAX + 1));
        if (blocked(nr)) continue;
        SYSCALL(nr, pick_arg(rng()), pick_arg(rng()), pick_arg(rng()), pick_arg(rng()),
                pick_arg(rng()), pick_arg(rng()));
    }
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int workers = 8;
    int rounds = 40;
    int per = 400;
    for (int sig = 1; sig <= 31; sig++)
        if (sig != 9 && sig != 19) signal(sig, SIG_IGN);
    print("fuzz: start\n");
    for (int round = 0; round < rounds; round++) {
        for (int w = 0; w < workers; w++) {
            s64 pid = fork();
            if (pid == 0) {
                worker((u64)(round * 1000003 + w * 7919 + 1), per);
                exit(0);
            }
        }
        for (int w = 0; w < workers; w++) {
            int status = 0;
            waitpid(-1, &status, 0);
        }
        if ((round & 31) == 0) {
            print("fuzz: round ");
            print_number(round);
            print("\n");
        }
    }
    print("fuzz: done\n");
    expect(1, "survived the fuzz run");
}
