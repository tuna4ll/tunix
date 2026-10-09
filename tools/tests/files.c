#define TEST_NAME "files"
#include "test.h"

static u8 block[64 * 1024];
static u8 check[64 * 1024];

static void fill(u8 *buffer, u64 length, u64 seed) {
    for (u64 i = 0; i < length; i++) buffer[i] = (u8)((i + seed) * 31 + (i >> 8));
}

static void directories(void) {
    expect_eq(mkdir("/tmp/files", 0755), 0, "mkdir creates a directory");
    expect_eq(mkdir("/tmp/files", 0755), -EEXIST, "a second mkdir says EEXIST");
}

static void read_and_write(void) {
    int fd = (int)open("/tmp/files/a", O_RDWR | O_CREAT | O_EXCL, 0644);
    expect(fd >= 0, "open with O_CREAT|O_EXCL creates a file");
    expect_eq(open("/tmp/files/a", O_RDWR | O_CREAT | O_EXCL, 0644), -EEXIST,
              "O_EXCL refuses an existing file");
    fill(block, 10000, 1);
    expect_eq(write(fd, block, 10000), 10000, "10000 bytes are written");
    expect_eq(fstat_size(fd), 10000, "fstat reports the new size");
    expect_eq(lseek(fd, 0, 0), 0, "lseek rewinds");
    expect_eq(read(fd, check, sizeof(check)), 10000, "a read returns all of it");
    expect(memeq(block, check, 10000), "and the bytes match");
    expect_eq(pread(fd, check, 100, 5000), 100, "pread reads from an offset");
    expect(memeq(block + 5000, check, 100), "at the right place");
    expect_eq(ftruncate(fd, 100), 0, "ftruncate shrinks the file");
    expect_eq(fstat_size(fd), 100, "to 100 bytes");
    close(fd);

    fd = (int)open("/tmp/files/a", O_WRONLY | O_APPEND, 0);
    write(fd, "tail", 4);
    close(fd);
    fd = (int)open("/tmp/files/a", O_RDONLY, 0);
    expect_eq(read(fd, check, sizeof(check)), 104, "O_APPEND writes after the end");
    expect(memeq(check + 100, "tail", 4), "and the appended bytes are there");
    close(fd);
}

static void large_file(void) {
    enum { CHUNKS = 64 };
    int fd = (int)open("/tmp/files/large", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int written = 1;
    for (u64 chunk = 0; chunk < CHUNKS; chunk++) {
        fill(block, sizeof(block), chunk);
        written &= write(fd, block, sizeof(block)) == (s64)sizeof(block);
    }
    expect(written, "4 MiB are written in 64 KiB chunks");
    expect_eq(fstat_size(fd), CHUNKS * sizeof(block), "the file is 4 MiB long");
    close(fd);

    fd = (int)open("/tmp/files/large", O_RDONLY, 0);
    int intact = 1;
    for (u64 chunk = 0; chunk < CHUNKS; chunk++) {
        fill(block, sizeof(block), chunk);
        intact &= read(fd, check, sizeof(check)) == (s64)sizeof(check);
        intact &= memeq(block, check, sizeof(block));
    }
    expect(intact, "reading it back after reopening gives the same 4 MiB");
    expect_eq(read(fd, check, sizeof(check)), 0, "a read at the end returns 0");
    close(fd);
}

static int directory_has(const char *path, const char *name) {
    static u8 buffer[4096];
    int fd = (int)open(path, O_RDONLY, 0);
    int found = 0;
    s64 length;
    while ((length = getdents64(fd, buffer, sizeof(buffer))) > 0) {
        for (s64 offset = 0; offset < length;) {
            struct dirent64 *entry = (struct dirent64 *)(buffer + offset);
            found |= streq(entry->name, name);
            offset += entry->record_length;
        }
    }
    close(fd);
    return found;
}

static void names(void) {
    expect(directory_has("/tmp/files", "a"), "getdents64 lists the file");
    expect_eq(rename("/tmp/files/a", "/tmp/files/b"), 0, "rename moves it");
    expect_eq(open("/tmp/files/a", O_RDONLY, 0), -ENOENT, "the old name is gone");
    expect(directory_has("/tmp/files", "b") && !directory_has("/tmp/files", "a"),
           "the directory shows only the new name");
    expect_eq(rmdir("/tmp/files"), -ENOTEMPTY, "rmdir refuses a full directory");
    expect_eq(unlink("/tmp/files/b"), 0, "unlink removes a file");
    expect_eq(unlink("/tmp/files/large"), 0, "and the large one");
    expect_eq(rmdir("/tmp/files"), 0, "rmdir removes the empty directory");
    expect_eq(open("/tmp/files/b", O_RDONLY, 0), -ENOENT, "nothing is left behind");
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    directories();
    read_and_write();
    large_file();
    names();
}
