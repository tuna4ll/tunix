/*
 * shm-test -- exercise the shared-memory primitives Wayland is built on.
 *
 * wl_shm works like this: a client creates an anonymous shareable file, sizes
 * it, maps it MAP_SHARED, draws into it, and passes the descriptor to the
 * compositor over a unix socket. The compositor maps the *same* descriptor and
 * must see the client's pixels. If any link in that chain copies instead of
 * shares, the compositor sees a blank buffer and nothing on screen is ever
 * right.
 *
 * The three cases below are exactly those links:
 *
 *   1. memfd + MAP_SHARED across fork      -- parent and child share memory
 *   2. MAP_SHARED|MAP_ANONYMOUS across fork -- the same, with no descriptor
 *   3. memfd passed over SCM_RIGHTS         -- the real wl_shm handoff
 *
 * Case 3 is the one that matters most: it is the only one where the receiving
 * process never inherited the mapping and has to reach the memory purely
 * through a descriptor it was handed.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHM_SIZE 65536
#define CLIENT_MARK 0xA5
#define SERVER_MARK 0x5C

static int failures;

static void check(int condition, const char *what) {
    if (condition) {
        printf("shm-test: ok   %s\n", what);
    } else {
        printf("shm-test: FAIL %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures++;
    }
}

static int make_memfd(size_t size) {
    /* Call the syscall directly: musl exposes memfd_create(), but going
       straight to the number keeps the test honest about what the kernel
       implements. */
    int fd = (int)syscall(SYS_memfd_create, "shm-test", 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 1. A memfd mapped before fork must be one buffer, not two. */
static void test_memfd_across_fork(void) {
    int fd = make_memfd(SHM_SIZE);
    check(fd >= 0, "memfd_create + ftruncate");
    if (fd < 0) return;

    unsigned char *map = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "mmap(MAP_SHARED) of a memfd");
    if (map == MAP_FAILED) { close(fd); return; }

    check(map[0] == 0 && map[SHM_SIZE - 1] == 0, "a fresh memfd reads as zero");

    memset(map, CLIENT_MARK, SHM_SIZE);
    pid_t pid = fork();
    if (pid == 0) {
        /* The child must see what the parent wrote before the fork, and its own
           write must be visible to the parent afterwards. */
        if (map[0] != CLIENT_MARK || map[SHM_SIZE - 1] != CLIENT_MARK) _exit(2);
        memset(map, SERVER_MARK, SHM_SIZE);
        _exit(0);
    }
    check(pid > 0, "fork");
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child sees the parent's writes through the memfd");
    check(map[0] == SERVER_MARK && map[SHM_SIZE - 1] == SERVER_MARK,
          "parent sees the child's writes through the memfd");

    munmap(map, SHM_SIZE);
    close(fd);
}

/* 2. The same guarantee without a descriptor behind it. */
static void test_anonymous_shared_across_fork(void) {
    unsigned char *map = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check(map != MAP_FAILED, "mmap(MAP_SHARED|MAP_ANONYMOUS)");
    if (map == MAP_FAILED) return;

    memset(map, CLIENT_MARK, SHM_SIZE);
    pid_t pid = fork();
    if (pid == 0) {
        if (map[0] != CLIENT_MARK) _exit(2);
        memset(map, SERVER_MARK, SHM_SIZE);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child sees the parent's anonymous shared writes");
    check(map[0] == SERVER_MARK && map[SHM_SIZE - 1] == SERVER_MARK,
          "anonymous shared mapping survives fork as shared");

    munmap(map, SHM_SIZE);
}

static int send_fd(int socket, int fd) {
    char body = 'f';
    struct iovec iov = { .iov_base = &body, .iov_len = 1 };
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct msghdr message = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control),
    };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(int));
    return sendmsg(socket, &message, 0) < 0 ? -1 : 0;
}

static int receive_fd(int socket) {
    char body = 0;
    struct iovec iov = { .iov_base = &body, .iov_len = 1 };
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct msghdr message = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control),
    };
    if (recvmsg(socket, &message, 0) < 0) return -1;
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(int));
    return fd;
}

/*
 * 3. The wl_shm handoff itself. The "client" creates and fills a buffer and
 * sends only the descriptor; the "compositor" maps what it receives. Nothing is
 * inherited -- the mapping is established from the descriptor alone.
 */
static void test_memfd_over_scm_rights(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        check(0, "socketpair");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pair[0]);
        int fd = make_memfd(SHM_SIZE);
        if (fd < 0) _exit(2);
        unsigned char *map = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) _exit(3);
        memset(map, CLIENT_MARK, SHM_SIZE);
        if (send_fd(pair[1], fd) != 0) _exit(4);
        /* Stay alive until the parent has looked, so the exit path cannot be
           what makes the memory visible. */
        char done;
        (void)read(pair[1], &done, 1);
        _exit(0);
    }
    close(pair[1]);

    int received = receive_fd(pair[0]);
    check(received >= 0, "receive a memfd over SCM_RIGHTS");
    if (received < 0) { close(pair[0]); return; }

    unsigned char *map = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED, received, 0);
    check(map != MAP_FAILED, "mmap a memfd that arrived over a socket");
    if (map != MAP_FAILED) {
        check(map[0] == CLIENT_MARK && map[SHM_SIZE - 1] == CLIENT_MARK,
              "the received mapping shows the sender's pixels");
        /* And the reverse direction, which is how a compositor returns a
           release or a client reads back damage. */
        map[0] = SERVER_MARK;
        munmap(map, SHM_SIZE);
    }

    (void)write(pair[0], "x", 1);
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "sender exited cleanly");
    close(received);
    close(pair[0]);
}

int main(void) {
    test_memfd_across_fork();
    test_anonymous_shared_across_fork();
    test_memfd_over_scm_rights();

    if (failures) {
        printf("shm-test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("shm-test: PASS\n");
    return 0;
}
