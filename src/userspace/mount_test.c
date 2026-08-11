/*
 * Does mounting change what the tree looks like?
 *
 * The question a mount has to answer is not "did the call return 0" but "does
 * the same path now reach somewhere else". So every check here is about what
 * a path resolves to: a file written under a mountpoint must vanish when a
 * filesystem is mounted over it and come back when it is unmounted, and one
 * written into the mounted tree must not survive the unmount.
 */
#include "tunix_libc.h"

#define MS_RDONLY 0x0001UL
#define MS_REMOUNT 0x0020UL
#define MS_BIND 0x1000UL

static int failures;

static void say(const char *text) {
    t_puts(text);
    int fd = t_open("/dev/kmsg", T_O_WRONLY, 0);
    if (fd < 0) return;
    (void)t_write(fd, text, t_strlen(text));
    t_close(fd);
}

static void check(int ok, const char *what) {
    say(ok ? "MOUNTTEST: ok   " : "MOUNTTEST: FAIL ");
    say(what);
    say("\n");
    if (!ok) failures++;
}

static int write_file(const char *path, const char *text) {
    int fd = t_open(path, T_O_WRONLY | T_O_CREAT | T_O_TRUNC, 0644);
    if (fd < 0) return -1;
    long written = t_write(fd, text, t_strlen(text));
    t_close(fd);
    return written == (long)t_strlen(text) ? 0 : -1;
}

static int file_exists(const char *path) {
    int fd = t_open(path, T_O_RDONLY, 0);
    if (fd < 0) return 0;
    t_close(fd);
    return 1;
}

/* Whether /proc/mounts has a line whose second field is this target. */
static int mounts_mention(const char *target) {
    int fd = t_open("/proc/mounts", T_O_RDONLY, 0);
    if (fd < 0) return 0;
    static char buffer[4096];
    long got = t_read(fd, buffer, sizeof(buffer) - 1);
    t_close(fd);
    if (got <= 0) return 0;
    buffer[got] = '\0';

    size_t length = t_strlen(target);
    for (long at = 0; at < got; at++) {
        /* Second field: step over the source, then compare. */
        if (at && buffer[at - 1] != '\n') continue;
        long field = at;
        while (field < got && buffer[field] != ' ' && buffer[field] != '\n') field++;
        if (field >= got || buffer[field] != ' ') continue;
        field++;
        if ((size_t)(got - field) < length) continue;
        int same = 1;
        for (size_t i = 0; i < length; i++)
            if (buffer[field + (long)i] != target[i]) same = 0;
        if (same && buffer[field + (long)length] == ' ') return 1;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    const char *point = "/mnt/mount-test";
    const char *under = "/mnt/mount-test/underneath";
    const char *inside = "/mnt/mount-test/inside";

    t_mkdir("/mnt", 0755);
    t_unlink(under);
    t_unlink(inside);
    t_mkdir(point, 0755);

    check(mounts_mention("/"), "/proc/mounts reports the root");
    check(mounts_mention("/proc"), "/proc/mounts reports itself");

    check(write_file(under, "underneath\n") == 0, "wrote a file at the mountpoint");
    check(t_mount("tmpfs", point, "tmpfs", 0, 0) == 0, "mounted a tmpfs over it");
    check(mounts_mention(point), "/proc/mounts reports the new mount");

    /* The point of a mount: the same path reaches somewhere else now. */
    check(!file_exists(under), "the file underneath is hidden");
    check(write_file(inside, "inside\n") == 0, "wrote a file in the mounted tree");
    check(file_exists(inside), "and can read it back");

    check(t_mount("tmpfs", point, "tmpfs", 0, 0) != 0, "mounting over it again was refused");
    check(t_mount("tmpfs", "/mnt/mount-test-missing", "tmpfs", 0, 0) != 0,
          "mounting on a path that does not exist was refused");
    check(t_mount("tmpfs", "/etc/passwd", "tmpfs", 0, 0) != 0,
          "mounting on a file was refused");
    check(t_mount("ext2", "/mnt", "ext2", 0, 0) != 0,
          "mounting a filesystem with no driver was refused");

    check(t_mount(0, point, "tmpfs", MS_REMOUNT | MS_RDONLY, 0) == 0, "remounted it");

    check(t_umount2(point, 0) == 0, "unmounted it");
    check(!mounts_mention(point), "/proc/mounts no longer reports it");
    check(!file_exists(inside), "the file inside went with the filesystem");
    check(file_exists(under), "the file underneath came back");

    check(t_umount2(point, 0) != 0, "unmounting it twice was refused");
    check(t_umount2("/proc", 0) != 0, "unmounting a system filesystem was refused");

    /* A bind mount shows a directory that already exists at a second place. */
    check(t_mount("/etc", point, "none", MS_BIND, 0) == 0, "bound /etc over it");
    check(file_exists("/mnt/mount-test/passwd"), "/etc/passwd is visible through it");
    check(t_umount2(point, 0) == 0, "unbound it");
    check(!file_exists("/mnt/mount-test/passwd"), "and it is gone again");
    check(file_exists("/etc/passwd"), "the original /etc is untouched");

    t_unlink(under);

    if (failures) {
        say("MOUNTTEST: FAILED\n");
        return 1;
    }
    say("MOUNTTEST: PASS mounting changes what a path reaches\n");
    return 0;
}
