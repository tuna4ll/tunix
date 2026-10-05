typedef unsigned long u64;
typedef long s64;

#include "tunix_syscall.h"

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_NANOSLEEP 35
#define SYS_GETPID 39
#define SYS_FORK 57
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_KILL 62
#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_STATFS 137
#define SYS_MOUNT 165
#define SYS_EXIT_GROUP 231
#define SYS_UMOUNT2 166
#define SYS_NEWFSTATAT 262
#define O_CREAT 0100
#define O_RDWR 2
#define MNT_DETACH 2
#define AT_FDCWD_TEST -100
#define SYS_INOTIFY_ADD_WATCH 254
#define SYS_INOTIFY_INIT1 294

#define O_RDONLY 0
#define O_WRONLY 1
#define IN_MODIFY 2
#define IN_NONBLOCK 04000
#define EBUSY 16
#define CGROUP2_SUPER_MAGIC 0x63677270L

struct timespec {
    s64 sec;
    s64 nsec;
};

static unsigned failures;
static char text[4096];

static void put(const char *value) {
    u64 length = 0;
    while (value[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)value, (s64)length);
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
    put("CGROUPTEST ");
    put(name);
    put(ok ? " PASS" : " FAIL ");
    if (!ok) put_number(detail);
    put("\n");
    if (!ok) failures++;
}

static s64 read_file(const char *path) {
    int fd = (int)syscall3(SYS_OPEN, (s64)path, O_RDONLY, 0);
    if (fd < 0) return fd;
    s64 total = 0;
    for (;;) {
        s64 got = syscall3(SYS_READ, fd, (s64)(text + total), (s64)(sizeof(text) - 1 - (u64)total));
        if (got <= 0) break;
        total += got;
    }
    text[total] = 0;
    (void)syscall1(SYS_CLOSE, fd);
    return total;
}

static s64 write_file(const char *path, const char *value) {
    int fd = (int)syscall3(SYS_OPEN, (s64)path, O_WRONLY, 0);
    if (fd < 0) return fd;
    u64 length = 0;
    while (value[length]) length++;
    s64 result = syscall3(SYS_WRITE, fd, (s64)value, (s64)length);
    (void)syscall1(SYS_CLOSE, fd);
    return result;
}

static int contains(const char *haystack, const char *needle) {
    for (u64 at = 0; haystack[at]; at++) {
        u64 index = 0;
        while (needle[index] && haystack[at + index] == needle[index]) index++;
        if (!needle[index]) return 1;
    }
    return 0;
}

static void number_text(s64 value, char *out) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
}

static int listed(const char *list, s64 pid) {
    char wanted[24];
    number_text(pid, wanted);
    u64 at = 0;
    while (list[at]) {
        u64 index = 0;
        while (wanted[index] && list[at + index] == wanted[index]) index++;
        if (!wanted[index] && (list[at + index] == '\n' || !list[at + index])) return 1;
        while (list[at] && list[at] != '\n') at++;
        if (list[at]) at++;
    }
    return 0;
}

static void nap(void) {
    struct timespec delay = {0, 20000000};
    (void)syscall2(SYS_NANOSLEEP, (s64)&delay, 0);
}

void run(void) {
    s64 self = syscall0(SYS_GETPID);
    char pid_text[24];
    number_text(self, pid_text);

    (void)syscall2(SYS_MKDIR, (s64)"/tmp/cg", 0755);
    s64 status = syscall6(SYS_MOUNT, (s64)"cgroup2", (s64)"/tmp/cg", (s64)"cgroup2", 0,
                          (s64)"nsdelegate", 0);
    check("mount-cgroup2", status == 0, status);

    s64 statfs_buffer[16];
    status = syscall2(SYS_STATFS, (s64)"/tmp/cg", (s64)statfs_buffer);
    check("statfs-magic", status == 0 && statfs_buffer[0] == CGROUP2_SUPER_MAGIC, statfs_buffer[0]);

    read_file("/tmp/cg/cgroup.procs");
    check("root-lists-self", listed(text, self), self);
    read_file("/proc/self/cgroup");
    check("proc-self-root", contains(text, "0::/\n"), 0);

    status = syscall2(SYS_MKDIR, (s64)"/tmp/cg/a", 0755);
    check("mkdir-child", status == 0, status);
    check("child-has-procs", read_file("/tmp/cg/a/cgroup.procs") == 0, 0);
    read_file("/tmp/cg/a/cgroup.events");
    check("child-empty", contains(text, "populated 0"), 0);

    int notify = (int)syscall1(SYS_INOTIFY_INIT1, IN_NONBLOCK);
    s64 watch = syscall3(SYS_INOTIFY_ADD_WATCH, notify, (s64)"/tmp/cg/a/cgroup.events", IN_MODIFY);
    check("watch-events", notify >= 0 && watch >= 0, watch);

    status = write_file("/tmp/cg/a/cgroup.procs", pid_text);
    check("move-self", status > 0, status);
    read_file("/proc/self/cgroup");
    check("proc-self-moved", contains(text, "0::/a\n"), 0);
    read_file("/tmp/cg/cgroup.procs");
    check("root-forgets-self", !listed(text, self), 0);
    read_file("/tmp/cg/a/cgroup.events");
    check("child-populated", contains(text, "populated 1"), 0);
    char event[256];
    s64 got = syscall3(SYS_READ, notify, (s64)event, sizeof(event));
    check("populated-notified", got > 0, got);

    s64 child = syscall0(SYS_FORK);
    if (child == 0) {
        for (;;) nap();
    }
    nap();
    char child_path[64] = "/proc/";
    number_text(child, child_path + 6);
    u64 end = 0;
    while (child_path[end]) end++;
    const char *suffix = "/cgroup";
    for (u64 index = 0; suffix[index]; index++) child_path[end++] = suffix[index];
    child_path[end] = 0;
    read_file(child_path);
    check("fork-inherits", contains(text, "0::/a\n"), child);

    status = syscall1(SYS_RMDIR, (s64)"/tmp/cg/a");
    check("rmdir-busy", status == -EBUSY, status);

    status = write_file("/tmp/cg/cgroup.procs", pid_text);
    check("move-back", status > 0, status);
    read_file("/tmp/cg/a/cgroup.events");
    check("child-keeps-it-populated", contains(text, "populated 1"), 0);
    while (syscall3(SYS_READ, notify, (s64)event, sizeof(event)) > 0) { }

    status = write_file("/tmp/cg/a/cgroup.kill", "1");
    check("kill-write", status > 0, status);
    int wait_status = 0;
    s64 reaped = syscall4(SYS_WAIT4, child, (s64)&wait_status, 0, 0);
    check("kill-reaped", reaped == child && (wait_status & 0x7F) == 9, wait_status);
    got = syscall3(SYS_READ, notify, (s64)event, sizeof(event));
    check("empty-notified", got > 0, got);
    read_file("/tmp/cg/a/cgroup.events");
    check("child-empty-again", contains(text, "populated 0"), 0);

    status = syscall1(SYS_RMDIR, (s64)"/tmp/cg/a");
    check("rmdir-empty", status == 0, status);
    check("rmdir-gone", read_file("/tmp/cg/a/cgroup.procs") < 0, 0);

    (void)syscall2(SYS_MKDIR, (s64)"/tmp/v1", 0755);
    status = syscall6(SYS_MOUNT, (s64)"cgroup", (s64)"/tmp/v1", (s64)"cgroup", 0,
                      (s64)"none,name=elogind", 0);
    check("mount-named-v1", status == 0, status);
    read_file("/proc/self/cgroup");
    check("proc-self-v1", contains(text, ":name=elogind:/\n") && contains(text, "0::/\n"), 0);
    status = syscall2(SYS_MKDIR, (s64)"/tmp/v1/s", 0755);
    check("v1-mkdir", status == 0, status);
    status = write_file("/tmp/v1/s/cgroup.procs", pid_text);
    check("v1-move", status > 0, status);
    read_file("/proc/self/cgroup");
    check("v1-moved", contains(text, ":name=elogind:/s\n") && contains(text, "0::/\n"), 0);
    read_file("/tmp/v1/s/tasks");
    check("v1-tasks", listed(text, self), 0);

    status = syscall6(SYS_MOUNT, (s64)"cgroup", (s64)"/tmp/v1", (s64)"cgroup", 0,
                      (s64)"none,name=elogind", 0);
    check("remount-refused", status != 0, status);

    (void)syscall2(SYS_MKDIR, (s64)"/tmp/rt", 0755);
    status = syscall6(SYS_MOUNT, (s64)"none", (s64)"/tmp/rt", (s64)"tmpfs", 0,
                      (s64)"nosuid,nodev,mode=0700,uid=993,gid=992,size=1M", 0);
    unsigned stat_buffer[36];
    s64 statted = syscall4(SYS_NEWFSTATAT, AT_FDCWD_TEST, (s64)"/tmp/rt", (s64)stat_buffer, 0);
#if defined(__x86_64__)
    unsigned mode = stat_buffer[6], uid = stat_buffer[7], gid = stat_buffer[8];
#else
    unsigned mode = stat_buffer[4], uid = stat_buffer[6], gid = stat_buffer[7];
#endif
    check("tmpfs-options", status == 0 && statted == 0 && (mode & 07777) == 0700 && uid == 993 &&
          gid == 992, (s64)(mode & 07777) * 100000 + uid);

    int below = (int)syscall3(SYS_OPEN, (s64)"/tmp/rt/below", O_CREAT | O_RDWR, 0600);
    status = syscall6(SYS_MOUNT, (s64)"none", (s64)"/tmp/rt", (s64)"tmpfs", 0, (s64)"mode=0755", 0);
    check("mount-stacks", status == 0 && read_file("/tmp/rt/below") < 0, status);
    status = syscall2(SYS_UMOUNT2, (s64)"/tmp/rt", 0);
    check("umount-top", status == 0 && read_file("/tmp/rt/below") == 0, status);
    status = syscall2(SYS_UMOUNT2, (s64)"/tmp/rt", 0);
    check("umount-busy", status == -EBUSY, status);
    status = syscall2(SYS_UMOUNT2, (s64)"/tmp/rt", MNT_DETACH);
    check("umount-detach", status == 0, status);
    (void)syscall1(SYS_CLOSE, below);

    put(failures ? "CGROUPTEST FAIL\n" : "CGROUPTEST PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
