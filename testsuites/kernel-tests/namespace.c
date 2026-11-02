#define TEST_NAME "namespace"
#include "test.h"

#if defined(__x86_64__)
#define SYS_unshare_nr  272
#define SYS_getuid_nr   102
#define SYS_geteuid_nr  107
#define SYS_getgid_nr   104
#define SYS_sethostname 170
#define SYS_uname_nr    63
#define SYS_openat_nr   257
#define SYS_setuid_nr   105
#define SYS_socket_nr_x 41
#define SYS_bind_nr_x   49
#define SYS_sendto_nr_x 44
#else
#define SYS_unshare_nr  97
#define SYS_getuid_nr   174
#define SYS_geteuid_nr  175
#define SYS_getgid_nr   176
#define SYS_sethostname 161
#define SYS_uname_nr    160
#define SYS_openat_nr   56
#define SYS_setuid_nr   146
#define SYS_socket_nr_x 198
#define SYS_bind_nr_x   200
#define SYS_sendto_nr_x 206
#endif

#define CLONE_NEWNS   0x00020000UL
#define CLONE_NEWUTS  0x04000000UL
#define CLONE_NEWUSER 0x10000000UL
#define CLONE_NEWNET  0x40000000UL
#define EINVAL        22
#define EPERM         1
#define O_WRONLY      1
#define O_RDONLY      0

static s64 unshare(u64 flags) { return SYSCALL(SYS_unshare_nr, flags, 0, 0, 0, 0, 0); }
static s64 raw_getuid(void) { return SYSCALL(SYS_getuid_nr, 0, 0, 0, 0, 0, 0); }
static s64 raw_geteuid(void) { return SYSCALL(SYS_geteuid_nr, 0, 0, 0, 0, 0, 0); }

static s64 write_file(const char *path, const char *text) {
    int fd = (int)SYSCALL(SYS_openat_nr, -100, path, O_WRONLY, 0, 0, 0);
    if (fd < 0) return fd;
    s64 n = write(fd, text, strlen(text));
    close(fd);
    return n;
}

static s64 read_file(const char *path, char *buffer, u64 cap) {
    int fd = (int)SYSCALL(SYS_openat_nr, -100, path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;
    s64 n = read(fd, buffer, cap);
    close(fd);
    return n;
}

static int child(s64 (*body)(void)) {
    s64 pid = fork();
    if (pid == 0) exit((int)body());
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

static s64 userns_maps_root(void) {
    if (SYSCALL(SYS_setuid_nr, 1000, 0, 0, 0, 0, 0) != 0) return 79;
    if (unshare(CLONE_NEWUSER) != 0) return 80;
    s64 w = write_file("/proc/self/uid_map", "0 1000 1");
    if (w <= 0) return 200 + (int)(-w);
    if (raw_geteuid() != 0) return 84;
    if (raw_getuid() != 0) return 85;
    return 0;
}

static s64 uid_map_reads_back(void) {
    if (unshare(CLONE_NEWUSER) != 0) return 86;
    if (write_file("/proc/self/uid_map", "0 1000 1") <= 0) return 87;
    char buffer[64];
    s64 n = read_file("/proc/self/uid_map", buffer, sizeof(buffer) - 1);
    if (n <= 0) return 88;
    buffer[n] = '\0';
    int ok = buffer[0] == '0';
    return ok ? 0 : 89;
}

static s64 map_before_userns_fails(void) {
    return write_file("/proc/self/uid_map", "0 0 1") > 0 ? 70 : 0;
}

static s64 double_map_fails(void) {
    if (unshare(CLONE_NEWUSER) != 0) return 71;
    if (write_file("/proc/self/uid_map", "0 1000 1") <= 0) return 72;
    return write_file("/proc/self/uid_map", "0 1000 1") > 0 ? 73 : 0;
}

static s64 uts_isolated(void) {
    if (unshare(CLONE_NEWUSER | CLONE_NEWUTS) != 0) return 74;
    const char *name = "sandbox";
    if (SYSCALL(SYS_sethostname, name, strlen(name), 0, 0, 0, 0) != 0) return 75;
    struct {
        char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65];
    } uts;
    if (SYSCALL(SYS_uname_nr, &uts, 0, 0, 0, 0, 0) != 0) return 76;
    return streq(uts.nodename, "sandbox") ? 0 : 77;
}

#define CLONE_NEWPID 0x20000000UL
#define CLONE_NEWIPC 0x08000000UL
static s64 unsupported_ns_rejected(void) {
    return unshare(CLONE_NEWIPC) == -EINVAL && unshare(CLONE_NEWPID) == -EINVAL ? 0 : 60;
}

static s64 parent_hostname_unchanged(void) {
    child(uts_isolated);
    struct {
        char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65];
    } uts;
    SYSCALL(SYS_uname_nr, &uts, 0, 0, 0, 0, 0);
    return streq(uts.nodename, "sandbox") ? 50 : 0;
}

struct sa_in {
    u16 family;
    u16 port;
    u32 addr;
    u8 zero[8];
};

static s64 netns_isolates(void) {
    if (unshare(CLONE_NEWNET) != 0) return 40;
    int fd = (int)SYSCALL(SYS_socket_nr_x, 2, 2, 0, 0, 0, 0);
    if (fd < 0) return 41;
    struct sa_in lo = {2, 0, 0x0100007f, {0}};
    lo.port = (u16)((9999 >> 8) | (9999 << 8));
    if (SYSCALL(SYS_bind_nr_x, fd, &lo, sizeof(lo), 0, 0, 0) != 0) return 42;
    struct sa_in ext = {2, 0, 0x08080808, {0}};
    ext.port = (u16)((53 >> 8) | (53 << 8));
    s64 r = SYSCALL(SYS_sendto_nr_x, fd, "x", 1, 0, &ext, sizeof(ext));
    if (r >= 0) return 43;
    struct sa_in self = {2, 0, 0x0100007f, {0}};
    self.port = (u16)((9999 >> 8) | (9999 << 8));
    if (SYSCALL(SYS_sendto_nr_x, fd, "hi", 2, 0, &self, sizeof(self)) != 2) return 44;
    close(fd);
    return 0;
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    expect_eq(exited_with(child(userns_maps_root)), 0,
              "a user namespace maps the unprivileged uid to root");
    expect_eq(exited_with(child(uid_map_reads_back)), 0, "the uid map reads back");
    expect_eq(exited_with(child(map_before_userns_fails)), 0,
              "writing uid_map without a user namespace fails");
    expect_eq(exited_with(child(double_map_fails)), 0, "the uid map cannot be written twice");
    expect_eq(exited_with(child(uts_isolated)), 0, "a UTS namespace has its own hostname");
    expect_eq(exited_with(child(unsupported_ns_rejected)), 0,
              "ipc and pid(unshare) namespaces are refused, not faked");
    expect_eq(exited_with(child(parent_hostname_unchanged)), 0,
              "the child's hostname does not leak to the parent");
    expect_eq(exited_with(child(netns_isolates)), 0,
              "a net namespace blocks external sends and keeps its own loopback");
}
