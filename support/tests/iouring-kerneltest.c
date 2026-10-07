typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_CLOSE 3
#define SYS_MMAP 9
#define SYS_NANOSLEEP 35
#define SYS_FORK 57
#define SYS_EXIT 60
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231
#define SYS_WAITID 247
#define SYS_EVENTFD2 290
#define SYS_PIPE2 293
#define SYS_IO_URING_SETUP 425
#define SYS_IO_URING_ENTER 426
#define SYS_IO_URING_REGISTER 427
#define SYS_PIDFD_OPEN 434

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_POPULATE 0x8000

#define OP_NOP 0
#define OP_POLL_ADD 6
#define OP_TIMEOUT 11
#define OP_TIMEOUT_REMOVE 12
#define OP_ASYNC_CANCEL 14
#define OP_LINK_TIMEOUT 15
#define OP_CLOSE 19
#define OP_READ 22
#define OP_WRITE 23

#define SQE_IO_LINK 4
#define SQE_CQE_SKIP_SUCCESS 64
#define ENTER_GETEVENTS 1
#define ENTER_EXT_ARG 8
#define TIMEOUT_ABS 1
#define FEAT_SINGLE_MMAP 1
#define SQ_CQ_OVERFLOW 2

#define EBADF 9
#define EAGAIN 11
#define EINVAL 22
#define ENOENT 2
#define ETIME 62
#define ECANCELED 125
#define POLLIN 1
#define P_PIDFD 3
#define WEXITED 4
#define O_NONBLOCK 04000

struct sq_offsets { u32 head, tail, ring_mask, ring_entries, flags, dropped, array, resv1; u64 user_addr; };
struct cq_offsets { u32 head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1; u64 user_addr; };
struct params {
    u32 sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd, resv[3];
    struct sq_offsets sq_off;
    struct cq_offsets cq_off;
};

struct sqe {
    u8 opcode, flags;
    u16 ioprio;
    int fd;
    u64 off, addr;
    u32 len, op_flags;
    u64 user_data;
    u16 buf_index, personality;
    int splice_fd_in;
    u64 addr3, pad;
};

struct cqe {
    u64 user_data;
    int res;
    u32 flags;
};

struct timespec64 { s64 sec, nsec; };
struct getevents_arg { u64 sigmask; u32 sigmask_sz; u32 min_wait; u64 ts; };

struct ring {
    int fd;
    struct params p;
    u8 *map;
    struct sqe *sqes;
    u32 sq_tail;
};

static unsigned failures;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)text, (s64)length);
}

static void put_number(s64 value) {
    char digits[24];
    int count = 0;
    u64 magnitude = value < 0 ? (u64)-value : (u64)value;
    do {
        digits[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    if (value < 0) put("-");
    char out[24];
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
    put(out);
}

static void check(const char *name, int ok, s64 detail) {
    put("IOURING ");
    put(name);
    put(ok ? " PASS" : " FAIL ");
    if (!ok) put_number(detail);
    put("\n");
    if (!ok) failures++;
}

static void zero(void *pointer, u64 length) {
    u8 *bytes = pointer;
    for (u64 index = 0; index < length; index++) bytes[index] = 0;
}

static s64 now_ns(void) {
    struct timespec64 value;
    syscall2(SYS_CLOCK_GETTIME, 1, (s64)&value);
    return value.sec * 1000000000L + value.nsec;
}

static void sleep_ms(s64 ms) {
    struct timespec64 value = {ms / 1000, (ms % 1000) * 1000000L};
    syscall2(SYS_NANOSLEEP, (s64)&value, 0);
}

static volatile u32 *u32_at(struct ring *ring, u32 offset) {
    return (volatile u32 *)(ring->map + offset);
}

static int ring_init(struct ring *ring, u32 entries) {
    zero(ring, sizeof(*ring));
    ring->fd = (int)syscall2(SYS_IO_URING_SETUP, entries, (s64)&ring->p);
    if (ring->fd < 0) return ring->fd;
    u64 size = ring->p.sq_off.array + ring->p.sq_entries * 4U;
    u64 cq_size = ring->p.cq_off.cqes + ring->p.cq_entries * sizeof(struct cqe);
    if (cq_size > size) size = cq_size;
    s64 map = syscall6(SYS_MMAP, 0, (s64)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                       ring->fd, 0);
    if (map < 0) return (int)map;
    s64 sqes = syscall6(SYS_MMAP, 0, (s64)(ring->p.sq_entries * sizeof(struct sqe)),
                        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring->fd, 0x10000000L);
    if (sqes < 0) return (int)sqes;
    ring->map = (u8 *)map;
    ring->sqes = (struct sqe *)sqes;
    return 0;
}

static struct sqe *get_sqe(struct ring *ring) {
    u32 mask = *u32_at(ring, ring->p.sq_off.ring_mask);
    u32 index = ring->sq_tail & mask;
    struct sqe *sqe = &ring->sqes[index];
    zero(sqe, sizeof(*sqe));
    ((volatile u32 *)(ring->map + ring->p.sq_off.array))[index] = index;
    ring->sq_tail++;
    return sqe;
}

static s64 submit(struct ring *ring, u32 wait, u32 flags, s64 arg, s64 argsz) {
    u32 head = __atomic_load_n(u32_at(ring, ring->p.sq_off.head), __ATOMIC_ACQUIRE);
    __atomic_store_n(u32_at(ring, ring->p.sq_off.tail), ring->sq_tail, __ATOMIC_RELEASE);
    return syscall6(SYS_IO_URING_ENTER, ring->fd, ring->sq_tail - head, wait,
                    flags | (wait ? ENTER_GETEVENTS : 0), arg, argsz);
}

static int reap(struct ring *ring, struct cqe *out, int capacity) {
    u32 head = *u32_at(ring, ring->p.cq_off.head);
    u32 tail = __atomic_load_n(u32_at(ring, ring->p.cq_off.tail), __ATOMIC_ACQUIRE);
    u32 mask = *u32_at(ring, ring->p.cq_off.ring_mask);
    struct cqe *cqes = (struct cqe *)(ring->map + ring->p.cq_off.cqes);
    int count = 0;
    while (head != tail && count < capacity) out[count++] = cqes[head++ & mask];
    __atomic_store_n(u32_at(ring, ring->p.cq_off.head), head, __ATOMIC_RELEASE);
    return count;
}

static struct cqe *find(struct cqe *list, int count, u64 user_data) {
    for (int index = 0; index < count; index++)
        if (list[index].user_data == user_data) return &list[index];
    return 0;
}

static s64 result_of(struct cqe *list, int count, u64 user_data) {
    struct cqe *entry = find(list, count, user_data);
    return entry ? entry->res : -99999;
}

static struct sqe *prep(struct sqe *sqe, u8 opcode, int fd, u64 addr, u32 len, u64 off,
                        u64 user_data) {
    sqe->opcode = opcode;
    sqe->fd = fd;
    sqe->addr = addr;
    sqe->len = len;
    sqe->off = off;
    sqe->user_data = user_data;
    return sqe;
}

static void child_write_after(int fd, s64 ms, const void *data, s64 length) {
    if (syscall0(SYS_FORK) == 0) {
        sleep_ms(ms);
        syscall3(SYS_WRITE, fd, (s64)data, length);
        syscall1(SYS_EXIT, 0);
    }
}

void run(void) {
    struct ring ring;
    struct cqe cqes[64];
    int status = ring_init(&ring, 8);
    check("setup", status == 0, status);
    if (status != 0) {
        put("IOURING FAIL\n");
        syscall1(SYS_EXIT_GROUP, 1);
    }
    check("features", (ring.p.features & FEAT_SINGLE_MMAP) && ring.p.sq_entries == 8 &&
          ring.p.cq_entries == 16, ring.p.features);
    check("ring-entries", *u32_at(&ring, ring.p.sq_off.ring_entries) == 8 &&
          *u32_at(&ring, ring.p.cq_off.ring_entries) == 16, 0);

    struct params bad;
    zero(&bad, sizeof(bad));
    bad.flags = 2;
    s64 result = syscall2(SYS_IO_URING_SETUP, 8, (s64)&bad);
    check("sqpoll-refused", result == -EINVAL, result);

    (void)prep(get_sqe(&ring), OP_NOP, -1, 0, 0, 0, 1);
    result = submit(&ring, 1, 0, 0, 0);
    int count = reap(&ring, cqes, 64);
    check("nop", result == 1 && count == 1 && cqes[0].user_data == 1 && cqes[0].res == 0, result);

    s64 start = now_ns();
    struct timespec64 when = {(start + 50000000L) / 1000000000L, (start + 50000000L) % 1000000000L};
    struct sqe *sqe = get_sqe(&ring);
    (void)prep(sqe, OP_TIMEOUT, -1, (u64)&when, 1, 0, 2);
    sqe->op_flags = TIMEOUT_ABS;
    result = submit(&ring, 1, 0, 0, 0);
    s64 elapsed = now_ns() - start;
    count = reap(&ring, cqes, 64);
    check("timeout-abs", result == 1 && count == 1 && cqes[0].res == -ETIME, cqes[0].res);
    check("timeout-waited", elapsed >= 45000000L && elapsed < 1000000000L, elapsed);

    start = now_ns();
    struct timespec64 unnormalised = {0, 1020000000L};
    (void)prep(get_sqe(&ring), OP_TIMEOUT, -1, (u64)&unnormalised, 1, 0, 30);
    struct timespec64 forever = {0x7fffffffffffffffL, 0x7fffffffffffffffL};
    sqe = get_sqe(&ring);
    (void)prep(sqe, OP_TIMEOUT, -1, (u64)&forever, 1, 0, 31);
    sqe->op_flags = TIMEOUT_ABS;
    result = submit(&ring, 1, 0, 0, 0);
    elapsed = now_ns() - start;
    count = reap(&ring, cqes, 64);
    check("timeout-unnormalised", result == 2 && count == 1 && cqes[0].user_data == 30 &&
          cqes[0].res == -ETIME && elapsed >= 1000000000L, cqes[0].res);
    (void)prep(get_sqe(&ring), OP_TIMEOUT_REMOVE, -1, 31, 0, 0, 32);
    submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("timeout-forever-removed", count == 2 && result_of(cqes, count, 31) == -ECANCELED, count);

    struct timespec64 later = {10, 0};
    (void)prep(get_sqe(&ring), OP_TIMEOUT, -1, (u64)&later, 1, 0, 3);
    submit(&ring, 0, 0, 0, 0);
    (void)prep(get_sqe(&ring), OP_TIMEOUT_REMOVE, -1, 3, 0, 0, 4);
    submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("timeout-remove", count == 2 && result_of(cqes, count, 3) == -ECANCELED &&
          result_of(cqes, count, 4) == 0, count);

    int event = (int)syscall2(SYS_EVENTFD2, 0, 0);
    u64 value = 0;
    u64 wide[4] = {0, 0, 0, 0};
    (void)prep(get_sqe(&ring), OP_READ, event, (u64)wide, sizeof(wide), (u64)-1, 5);
    start = now_ns();
    u64 seven = 7;
    child_write_after(event, 30, &seven, 8);
    result = submit(&ring, 1, 0, 0, 0);
    elapsed = now_ns() - start;
    count = reap(&ring, cqes, 64);
    check("eventfd-read-blocks", count == 1 && cqes[0].res == 8 && wide[0] == 7 &&
          elapsed >= 25000000L, elapsed);

    int pipefd[2];
    syscall2(SYS_PIPE2, (s64)pipefd, 0);
    prep(get_sqe(&ring), OP_POLL_ADD, pipefd[0], 0, 0, 0, 6)->op_flags = POLLIN;
    char text[16];
    zero(text, sizeof(text));
    (void)prep(get_sqe(&ring), OP_READ, pipefd[0], (u64)text, sizeof(text), 0, 7);
    child_write_after(pipefd[1], 20, "hello", 5);
    result = submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("poll-and-pipe-read", result == 2 && count == 2 &&
          (result_of(cqes, count, 6) & POLLIN) && result_of(cqes, count, 7) == 5 &&
          text[0] == 'h' && text[4] == 'o', count);

    (void)prep(get_sqe(&ring), OP_READ, pipefd[0], (u64)text, sizeof(text), 0, 8);
    submit(&ring, 0, 0, 0, 0);
    (void)prep(get_sqe(&ring), OP_ASYNC_CANCEL, -1, 8, 0, 0, 9);
    submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("async-cancel", count == 2 && result_of(cqes, count, 8) == -ECANCELED &&
          result_of(cqes, count, 9) == 0, count);
    (void)prep(get_sqe(&ring), OP_ASYNC_CANCEL, -1, 12345, 0, 0, 10);
    submit(&ring, 1, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("cancel-missing", count == 1 && cqes[0].res == -ENOENT, cqes[0].res);

    zero(text, sizeof(text));
    prep(get_sqe(&ring), OP_WRITE, pipefd[1], (u64)"chain", 5, 0, 11)->flags = SQE_IO_LINK;
    (void)prep(get_sqe(&ring), OP_READ, pipefd[0], (u64)text, sizeof(text), 0, 12);
    result = submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("link-write-read", count == 2 && result_of(cqes, count, 11) == 5 &&
          result_of(cqes, count, 12) == 5 && text[0] == 'c', count);

    prep(get_sqe(&ring), OP_READ, 999, (u64)text, 1, 0, 13)->flags = SQE_IO_LINK;
    (void)prep(get_sqe(&ring), OP_NOP, -1, 0, 0, 0, 14);
    submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("link-failure-cancels", count == 2 && result_of(cqes, count, 13) == -EBADF &&
          result_of(cqes, count, 14) == -ECANCELED, count);

    struct timespec64 short_wait = {0, 30000000L};
    prep(get_sqe(&ring), OP_READ, pipefd[0], (u64)text, sizeof(text), 0, 15)->flags = SQE_IO_LINK;
    (void)prep(get_sqe(&ring), OP_LINK_TIMEOUT, -1, (u64)&short_wait, 1, 0, 16);
    submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("link-timeout", count == 2 && result_of(cqes, count, 15) == -ECANCELED &&
          result_of(cqes, count, 16) == -ETIME, count);

    prep(get_sqe(&ring), OP_NOP, -1, 0, 0, 0, 17)->flags = SQE_CQE_SKIP_SUCCESS;
    (void)prep(get_sqe(&ring), OP_NOP, -1, 0, 0, 0, 18);
    submit(&ring, 1, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("skip-success", count == 1 && cqes[0].user_data == 18, count);

    for (int round = 0; round < 3; round++) {
        for (int index = 0; index < 8; index++)
            prep(get_sqe(&ring), OP_NOP, -1, 0, 0, 0, 100 + (u64)(round * 8 + index));
        submit(&ring, 0, 0, 0, 0);
    }
    u32 sq_flags = *u32_at(&ring, ring.p.sq_off.flags);
    count = reap(&ring, cqes, 64);
    int total = count;
    syscall6(SYS_IO_URING_ENTER, ring.fd, 0, 0, ENTER_GETEVENTS, 0, 0);
    total += reap(&ring, cqes, 64);
    sq_flags = sq_flags & SQ_CQ_OVERFLOW;
    check("cq-overflow-kept", count == 16 && total == 24 && sq_flags &&
          !(*u32_at(&ring, ring.p.sq_off.flags) & SQ_CQ_OVERFLOW), total);

    (void)prep(get_sqe(&ring), OP_READ, pipefd[0], (u64)text, sizeof(text), 0, 19);
    struct timespec64 wait_for = {0, 40000000L};
    struct getevents_arg arg = {0, 8, 0, (u64)&wait_for};
    start = now_ns();
    result = submit(&ring, 0, 0, 0, 0);
    result = syscall6(SYS_IO_URING_ENTER, ring.fd, 0, 1, ENTER_GETEVENTS | ENTER_EXT_ARG,
                      (s64)&arg, sizeof(arg));
    elapsed = now_ns() - start;
    check("ext-arg-timeout", result == -ETIME && elapsed >= 35000000L, result);
    syscall3(SYS_WRITE, pipefd[1], (s64)"z", 1);
    syscall6(SYS_IO_URING_ENTER, ring.fd, 0, 1, ENTER_GETEVENTS, 0, 0);
    count = reap(&ring, cqes, 64);
    check("read-after-wait", count == 1 && cqes[0].res == 1 && text[0] == 'z', count);

    s64 child = syscall0(SYS_FORK);
    if (child == 0) {
        sleep_ms(30);
        syscall1(SYS_EXIT, 42);
    }
    int pidfd = (int)syscall2(SYS_PIDFD_OPEN, child, O_NONBLOCK);
    check("pidfd-open", pidfd >= 0, pidfd);
    u8 info[128];
    result = syscall6(SYS_WAITID, P_PIDFD, pidfd, (s64)info, WEXITED, 0, 0);
    check("pidfd-waitid-nonblock", result == -EAGAIN, result);
    prep(get_sqe(&ring), OP_POLL_ADD, pidfd, 0, 0, 0, 20)->op_flags = POLLIN;
    result = submit(&ring, 1, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    check("pidfd-poll", count == 1 && (cqes[0].res & POLLIN), cqes[0].res);
    zero(info, sizeof(info));
    result = syscall6(SYS_WAITID, P_PIDFD, pidfd, (s64)info, WEXITED, 0, 0);
    int *fields = (int *)info;
    check("pidfd-waitid", result == 0 && fields[4] == (int)child && fields[6] == 42, result);
    result = syscall2(SYS_PIDFD_OPEN, 999999, 0);
    check("pidfd-missing", result == -3, result);

    (void)prep(get_sqe(&ring), OP_CLOSE, event, 0, 0, 0, 21);
    (void)prep(get_sqe(&ring), OP_CLOSE, ring.fd, 0, 0, 0, 22);
    submit(&ring, 2, 0, 0, 0);
    count = reap(&ring, cqes, 64);
    result = syscall3(SYS_READ, event, (s64)&value, 8);
    check("close", count == 2 && result_of(cqes, count, 21) == 0 &&
          result_of(cqes, count, 22) == -EBADF && result == -EBADF, result);

    u8 probe[16 + 8 * 64];
    zero(probe, sizeof(probe));
    result = syscall4(SYS_IO_URING_REGISTER, ring.fd, 8, (s64)probe, 64);
    check("probe", result == 0 && probe[1] == 58 && (probe[16 + 8 * OP_READ + 2] & 1) &&
          !(probe[16 + 8 * 4 + 2] & 1), result);

    syscall1(SYS_CLOSE, ring.fd);
    put(failures ? "IOURING FAIL\n" : "IOURING PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
