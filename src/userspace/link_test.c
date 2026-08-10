/*
 * Are two names for one file really one file?
 *
 * A hard link is not observable by looking at either name on its own: both
 * report a file of the right size whether they share contents or merely happen
 * to hold the same bytes. So every check here is a comparison between the two
 * names -- same inode, same link count, a write through one seen through the
 * other -- and the last of them is the one that matters most: removing a name
 * must leave the file, and only the last name may take it away.
 *
 * It runs under /tmp, which is volatile, and again under /root, which is on the
 * disk, because the two go through completely different code: the first never
 * reaches the ext2 driver at all.
 */
#include "tunix_libc.h"

#define CONTENTS "hard link\n"

static int failures;

static void say(const char *text) {
    t_puts(text);
    int fd = t_open("/dev/kmsg", T_O_WRONLY, 0);
    if (fd < 0) return;
    (void)t_write(fd, text, t_strlen(text));
    t_close(fd);
}

static void check(int ok, const char *what) {
    say(ok ? "LINKTEST: ok   " : "LINKTEST: FAIL ");
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

static long read_file(const char *path, char *buffer, size_t size) {
    int fd = t_open(path, T_O_RDONLY, 0);
    if (fd < 0) return -1;
    long got = t_read(fd, buffer, size - 1);
    t_close(fd);
    if (got < 0) return -1;
    buffer[got] = '\0';
    return got;
}

static void join(char *out, const char *directory, const char *name) {
    size_t at = t_strlen(directory);
    t_memcpy(out, directory, at);
    out[at++] = '/';
    t_memcpy(out + at, name, t_strlen(name) + 1);
}

static void run(const char *directory) {
    char original[64], second[64], third[64], refused[64];
    char buffer[64];
    struct t_stat a, b;

    join(original, directory, "link-test-file");
    join(second, directory, "link-test-second");
    join(third, directory, "link-test-third");
    join(refused, directory, "link-test-refused");

    t_unlink(original);
    t_unlink(second);
    t_unlink(third);
    t_unlink(refused);

    say("LINKTEST: in ");
    say(directory);
    say("\n");

    check(write_file(original, CONTENTS) == 0, "wrote the file");
    check(t_link(original, second) == 0, "link() gave it a second name");
    check(t_link(original, third) == 0, "link() gave it a third name");
    check(t_link(original, second) < 0, "link() refused an existing name");
    /* A fresh name, so only the directory can be what makes this fail. */
    check(t_link(directory, refused) < 0, "link() refused a directory");
    check(t_stat(refused, &a) < 0, "the refused name was not created");

    check(t_stat(original, &a) == 0 && t_stat(second, &b) == 0,
          "both names stat");
    check(a.ino == b.ino, "both names are one inode");
    check(a.nlink == 3, "the file counts three names");
    check(a.size == b.size, "both names are one size");

    /* A write through the new name has to land in the same file, which is what
       separates a hard link from a copy. */
    check(write_file(second, "rewritten\n") == 0, "wrote through the link");
    check(read_file(original, buffer, sizeof(buffer)) > 0 &&
          t_strcmp(buffer, "rewritten\n") == 0,
          "the write is visible through the original");

    check(t_unlink(original) == 0, "removed the original name");
    check(read_file(second, buffer, sizeof(buffer)) > 0 &&
          t_strcmp(buffer, "rewritten\n") == 0,
          "the file outlived the name it was made under");
    check(t_stat(second, &a) == 0 && a.nlink == 2, "two names are left");

    check(t_unlink(second) == 0, "removed the second name");
    check(t_stat(third, &a) == 0 && a.nlink == 1, "one name is left");
    check(read_file(third, buffer, sizeof(buffer)) > 0 &&
          t_strcmp(buffer, "rewritten\n") == 0, "the last name still reads");

    check(t_unlink(third) == 0, "removed the last name");
    check(t_stat(third, &a) < 0, "the file is gone");
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    run("/tmp");
    run("/root");

    if (failures) {
        say("LINKTEST: FAILED\n");
        return 1;
    }
    say("LINKTEST: PASS hard links behave\n");
    return 0;
}
