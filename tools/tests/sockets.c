#define TEST_NAME "sockets"
#include "test.h"

static void stream(void) {
    int fds[2];
    expect_eq(socketpair(AF_UNIX, SOCK_STREAM, fds), 0, "socketpair makes a stream pair");
    s64 child = fork();
    if (child == 0) {
        close(fds[0]);
        char text[64];
        s64 count;
        while ((count = read(fds[1], text, sizeof(text))) > 0) {
            for (s64 i = 0; i < count; i++)
                if (text[i] >= 'a' && text[i] <= 'z') text[i] = (char)(text[i] - 32);
            write(fds[1], text, (u64)count);
        }
        exit(0);
    }
    close(fds[1]);
    char reply[16] = {0};
    write(fds[0], "tunix", 5);
    expect_eq(read(fds[0], reply, sizeof(reply)), 5, "a child answers over the socket");
    expect(memeq(reply, "TUNIX", 5), "with what it was sent, upper-cased");
    close(fds[0]);
    int status;
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 0, "closing one end ends the child's read loop");
}

static void datagrams(void) {
    int fds[2];
    expect_eq(socketpair(AF_UNIX, SOCK_DGRAM, fds), 0, "socketpair makes a datagram pair");
    write(fds[0], "a", 1);
    write(fds[0], "bcd", 3);
    write(fds[0], "efghij", 6);
    char text[16];
    s64 first = read(fds[1], text, sizeof(text));
    s64 second = read(fds[1], text, sizeof(text));
    s64 third = read(fds[1], text, sizeof(text));
    expect(first == 1 && second == 3 && third == 6, "each datagram keeps its own boundary");
    close(fds[0]);
    close(fds[1]);
}

static void passing_descriptors(void) {
    int sockets[2], pipe[2];
    socketpair(AF_UNIX, SOCK_STREAM, sockets);
    pipe2(pipe, 0);

    union {
        struct cmsghdr header;
        u8 bytes[sizeof(struct cmsghdr) + 8];
    } control;
    memset(&control, 0, sizeof(control));
    control.header.length = sizeof(struct cmsghdr) + sizeof(int);
    control.header.level = SOL_SOCKET;
    control.header.type = SCM_RIGHTS;
    *(int *)(control.bytes + sizeof(struct cmsghdr)) = pipe[1];

    char byte = 'x';
    struct iovec vector = {&byte, 1};
    struct msghdr message = {0, 0, &vector, 1, &control, sizeof(control), 0};
    expect_eq(sendmsg(sockets[0], &message, 0), 1, "sendmsg carries an fd with SCM_RIGHTS");
    close(pipe[1]);

    memset(&control, 0, sizeof(control));
    byte = 0;
    struct msghdr received = {0, 0, &vector, 1, &control, sizeof(control), 0};
    expect_eq(recvmsg(sockets[1], &received, 0), 1, "recvmsg takes the message");
    expect(byte == 'x' && control.header.type == SCM_RIGHTS, "with the rights attached");
    int passed = *(int *)(control.bytes + sizeof(struct cmsghdr));
    expect(passed >= 0, "the receiver gets a new fd for it");

    expect_eq(write(passed, "fd", 2), 2, "writing to the passed fd works");
    char text[2];
    expect_eq(read(pipe[0], text, 2), 2, "and reaches the original pipe");
    close(passed);
    expect_eq(read(pipe[0], text, 2), 0, "closing the last copy ends the pipe");
    close(pipe[0]);
    close(sockets[0]);
    close(sockets[1]);
}

static void socket_mode(void) {
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, fds);
    expect_eq(fchmod(fds[0], 0660), 0, "fchmod on a socket succeeds, as daemons expect");
    close(fds[0]);
    close(fds[1]);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    stream();
    datagrams();
    passing_descriptors();
    socket_mode();
}
