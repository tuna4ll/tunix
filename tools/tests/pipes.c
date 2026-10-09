#define TEST_NAME "pipes"
#include "test.h"

static u8 buffer[256 * 1024];

static void basic(void) {
    int fds[2];
    expect_eq(pipe2(fds, 0), 0, "pipe2 makes a pipe");
    expect_eq(write(fds[1], "hello", 5), 5, "five bytes go in");
    char text[8] = {0};
    expect_eq(read(fds[0], text, sizeof(text)), 5, "five bytes come out");
    expect(memeq(text, "hello", 5), "in order");
    close(fds[1]);
    expect_eq(read(fds[0], text, sizeof(text)), 0, "closing the writer gives EOF");
    close(fds[0]);
}

static void between_processes(void) {
    int fds[2];
    pipe2(fds, 0);
    s64 child = fork();
    if (child == 0) {
        close(fds[0]);
        for (u64 i = 0; i < sizeof(buffer); i++) buffer[i] = (u8)(i * 7);
        u64 sent = 0;
        while (sent < sizeof(buffer)) {
            s64 count = write(fds[1], buffer + sent, sizeof(buffer) - sent);
            if (count <= 0) exit(1);
            sent += (u64)count;
        }
        exit(0);
    }
    close(fds[1]);
    u64 received = 0;
    s64 count;
    while ((count = read(fds[0], buffer + received, sizeof(buffer) - received)) > 0)
        received += (u64)count;
    int intact = 1;
    for (u64 i = 0; i < sizeof(buffer); i++) intact &= buffer[i] == (u8)(i * 7);
    expect_eq((s64)received, sizeof(buffer),
              "256 KiB cross from a child, more than the pipe holds");
    expect(intact, "without a byte out of place");
    int status;
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "the writer finished cleanly");
    close(fds[0]);
}

static void nonblocking(void) {
    int fds[2];
    pipe2(fds, O_NONBLOCK);
    char byte;
    expect_eq(read(fds[0], &byte, 1), -EAGAIN, "an empty O_NONBLOCK pipe says EAGAIN");
    u64 total = 0;
    s64 count;
    while ((count = write(fds[1], buffer, sizeof(buffer))) > 0) total += (u64)count;
    expect_eq(count, -EAGAIN, "a full one says EAGAIN to a writer");
    expect(total >= 4096, "after it took at least a page");
    close(fds[0]);
    close(fds[1]);
}

static void broken_pipe(void) {
    int fds[2];
    pipe2(fds, 0);
    signal(SIGPIPE, SIG_IGN);
    close(fds[0]);
    expect_eq(write(fds[1], "x", 1), -EPIPE, "writing with no reader says EPIPE");
    close(fds[1]);
}

static void duplication(void) {
    int fds[2];
    pipe2(fds, 0);
    expect_eq(dup3(fds[1], 20, 0), 20, "dup3 copies the write end to fd 20");
    close(fds[1]);
    write(20, "dup", 3);
    char text[3];
    expect_eq(read(fds[0], text, 3), 3, "data written through the copy arrives");
    close(20);
    expect_eq(write(20, "x", 1), -EBADF, "a closed fd says EBADF");
    close(fds[0]);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    basic();
    between_processes();
    nonblocking();
    broken_pipe();
    duplication();
}
