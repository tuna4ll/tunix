#define TEST_NAME "mountns"
#include "test.h"

#if defined(__x86_64__)
#define SYS_mount_nr   165
#define SYS_mkdirat_nr 258
#define SYS_unshare_nr 272
#define SYS_openat_nr  257
#else
#define SYS_mount_nr   40
#define SYS_mkdirat_nr 34
#define SYS_unshare_nr 97
#define SYS_openat_nr  56
#endif

#define CLONE_NEWNS 0x00020000UL
#define O_CREAT_V   0100
#define O_WRONLY_V  1
#define O_RDONLY_V  0

static s64 mkdirp(const char *p) { return SYSCALL(SYS_mkdirat_nr, -100, p, 0755, 0, 0, 0); }
static s64 do_unshare(u64 f) { return SYSCALL(SYS_unshare_nr, f, 0, 0, 0, 0, 0); }
static s64 do_mount(const char *src, const char *tgt, const char *type) {
    return SYSCALL(SYS_mount_nr, src, tgt, type, 0, 0, 0);
}
static int exists(const char *p) {
    int fd = (int)SYSCALL(SYS_openat_nr, -100, p, O_RDONLY_V, 0, 0, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static s64 mount_isolated(void) {
    mkdirp("/tmp/m");
    s64 child = fork();
    if (child == 0) {
        if (do_unshare(CLONE_NEWNS) != 0) exit(40);
        if (do_mount("tmpfs", "/tmp/m", "tmpfs") != 0) exit(41);
        int fd =
            (int)SYSCALL(SYS_openat_nr, -100, "/tmp/m/inside", O_CREAT_V | O_WRONLY_V, 0644, 0, 0);
        if (fd < 0) exit(42);
        close(fd);
        if (!exists("/tmp/m/inside")) exit(43);
        exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (exited_with(status) != 0) return 100 + exited_with(status);
    return exists("/tmp/m/inside") ? 1 : 0;
}

static s64 parent_mount_invisible_to_child(void) {
    mkdirp("/tmp/p");
    s64 child = fork();
    if (child == 0) {
        if (do_unshare(CLONE_NEWNS) != 0) exit(44);
        s64 r = do_mount("tmpfs", "/tmp/p", "tmpfs");
        exit(r == 0 ? 0 : 45);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return exited_with(status);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    expect_eq(mount_isolated(), 0, "a mount in a namespace is not visible to the parent");
    expect_eq(parent_mount_invisible_to_child(), 0, "unshare(NEWNS) then mount succeeds privately");
}
