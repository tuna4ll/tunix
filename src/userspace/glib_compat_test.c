#include <stddef.h>
#include <stdint.h>
#include "tunix_libc.h"
#include <tunix/glib_compat.h>

#define CLOCK_MONOTONIC 1
#define TEST_DUP_FD 200
#define EVENT_DATA_EVENTFD 0x455644ULL
#define EVENT_DATA_TIMERFD 0x544644ULL
#define FD_STRESS_COUNT 80

static int failures;

static void pass(const char *name) {
    t_puts("GLIB-COMPAT: ");
    t_puts(name);
    t_puts(" PASS\n");
}

static void fail_value(const char *name, long value) {
    t_puts("GLIB-COMPAT: ");
    t_puts(name);
    t_puts(" FAIL (");
    t_print_long(value);
    t_puts(")\n");
    failures++;
}

static void close_if_open(int fd) {
    if (fd >= 0) (void)t_close(fd);
}

static int check_fd_flags(int fd, int expected_fd_flags, int expected_status_flags) {
    int fd_flags = t_fcntl(fd, T_F_GETFD, 0);
    int status_flags = t_fcntl(fd, T_F_GETFL, 0);
    if (fd_flags < 0 || status_flags < 0) return -1;
    if ((fd_flags & T_FD_CLOEXEC) != expected_fd_flags) return -2;
    if ((status_flags & T_O_NONBLOCK) != expected_status_flags) return -3;
    return 0;
}

static void test_pipe2_dup3(void) {
    int pipe_fds[2] = {-1, -1};
    int result = t_pipe2(pipe_fds, T_O_NONBLOCK | T_O_CLOEXEC);
    if (result < 0) {
        fail_value("pipe2", result);
        return;
    }
    result = check_fd_flags(pipe_fds[0], T_FD_CLOEXEC, T_O_NONBLOCK);
    if (result == 0)
        result = check_fd_flags(pipe_fds[1], T_FD_CLOEXEC, T_O_NONBLOCK);
    if (result != 0) {
        fail_value("pipe2 flags", result);
        close_if_open(pipe_fds[0]);
        close_if_open(pipe_fds[1]);
        return;
    }

    result = t_dup3(pipe_fds[0], TEST_DUP_FD, T_O_CLOEXEC);
    if (result != TEST_DUP_FD) {
        fail_value("dup3", result);
        close_if_open(pipe_fds[0]);
        close_if_open(pipe_fds[1]);
        return;
    }
    result = check_fd_flags(TEST_DUP_FD, T_FD_CLOEXEC, T_O_NONBLOCK);
    if (result != 0) {
        fail_value("dup3 flags", result);
    } else {
        pass("pipe2/dup3/cloexec");
    }
    close_if_open(TEST_DUP_FD);
    close_if_open(pipe_fds[0]);
    close_if_open(pipe_fds[1]);
}

static void test_fd_capacity(void) {
    int descriptors[FD_STRESS_COUNT];
    for (int index = 0; index < FD_STRESS_COUNT; index++) descriptors[index] = -1;
    int opened = 0;
    for (; opened < FD_STRESS_COUNT; opened++) {
        descriptors[opened] = t_eventfd(0, T_EFD_NONBLOCK | T_EFD_CLOEXEC);
        if (descriptors[opened] < 0) break;
    }
    for (int index = 0; index < opened; index++) close_if_open(descriptors[index]);
    if (opened == FD_STRESS_COUNT) pass("256 descriptor table");
    else fail_value("descriptor capacity", opened);
}

static void test_exec_cloexec(void) {
    int source = t_eventfd(0, T_EFD_CLOEXEC);
    if (source < 0) {
        fail_value("exec cloexec eventfd", source);
        return;
    }
    int result = t_dup3(source, TEST_DUP_FD, T_O_CLOEXEC);
    close_if_open(source);
    if (result != TEST_DUP_FD) {
        fail_value("exec cloexec dup3", result);
        return;
    }

    long child = t_fork();
    if (child < 0) {
        fail_value("exec cloexec fork", child);
        close_if_open(TEST_DUP_FD);
        return;
    }
    if (child == 0) {
        char *arguments[] = {"/bin/glib-compat-test", "--verify-cloexec", NULL};
        t_execve(arguments[0], arguments, t_environ);
        t_exit(127);
    }

    int status = 0;
    long waited;
    do {
        waited = t_waitpid(child, &status, 0);
    } while (waited < 0);
    close_if_open(TEST_DUP_FD);
    if (((status >> 8) & 0xff) == 0)
        pass("FD_CLOEXEC across execve");
    else
        fail_value("FD_CLOEXEC across execve", (status >> 8) & 0xff);
}

static void test_eventfd_epoll(void) {
    int event_fd = -1;
    int epoll_fd = -1;
    event_fd = t_eventfd(0, T_EFD_NONBLOCK | T_EFD_CLOEXEC);
    if (event_fd < 0) {
        fail_value("eventfd", event_fd);
        return;
    }
    epoll_fd = t_epoll_create1(T_EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        fail_value("epoll_create1", epoll_fd);
        close_if_open(event_fd);
        return;
    }

    struct t_epoll_event registration = {T_EPOLLIN, EVENT_DATA_EVENTFD};
    int result = t_epoll_ctl(epoll_fd, T_EPOLL_CTL_ADD, event_fd, &registration);
    if (result < 0) {
        fail_value("epoll_ctl eventfd", result);
        goto done;
    }
    struct t_epoll_event ready[2];
    t_memset(ready, 0, sizeof(ready));
    result = t_epoll_wait(epoll_fd, ready, 2, 0);
    if (result != 0) {
        fail_value("epoll empty", result);
        goto done;
    }
    uint64_t written = 7;
    long amount = t_write(event_fd, &written, sizeof(written));
    if (amount != (long)sizeof(written)) {
        fail_value("eventfd write", amount);
        goto done;
    }
    result = t_epoll_wait(epoll_fd, ready, 2, 100);
    if (result != 1 || ready[0].data != EVENT_DATA_EVENTFD ||
        !(ready[0].events & T_EPOLLIN)) {
        fail_value("epoll eventfd readiness", result);
        goto done;
    }
    uint64_t received = 0;
    amount = t_read(event_fd, &received, sizeof(received));
    if (amount != (long)sizeof(received) || received != written) {
        fail_value("eventfd read", amount < 0 ? amount : (long)received);
        goto done;
    }
    pass("eventfd + epoll");

done:
    close_if_open(epoll_fd);
    close_if_open(event_fd);
}

static void test_timerfd_epoll(void) {
    int timer_fd = t_timerfd_create(CLOCK_MONOTONIC,
                                    T_TFD_NONBLOCK | T_TFD_CLOEXEC);
    if (timer_fd < 0) {
        fail_value("timerfd_create", timer_fd);
        return;
    }
    int epoll_fd = t_epoll_create1(T_EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        fail_value("epoll timer create", epoll_fd);
        close_if_open(timer_fd);
        return;
    }
    struct t_epoll_event registration = {T_EPOLLIN, EVENT_DATA_TIMERFD};
    int result = t_epoll_ctl(epoll_fd, T_EPOLL_CTL_ADD, timer_fd, &registration);
    if (result < 0) {
        fail_value("epoll_ctl timerfd", result);
        goto done;
    }
    struct t_itimerspec timer;
    t_memset(&timer, 0, sizeof(timer));
    timer.value.tv_nsec = 20000000;
    result = t_timerfd_settime(timer_fd, 0, &timer, NULL);
    if (result < 0) {
        fail_value("timerfd_settime", result);
        goto done;
    }
    struct t_epoll_event ready;
    t_memset(&ready, 0, sizeof(ready));
    result = t_epoll_wait(epoll_fd, &ready, 1, 500);
    if (result != 1 || ready.data != EVENT_DATA_TIMERFD ||
        !(ready.events & T_EPOLLIN)) {
        fail_value("timerfd epoll readiness", result);
        goto done;
    }
    uint64_t expirations = 0;
    long amount = t_read(timer_fd, &expirations, sizeof(expirations));
    if (amount != (long)sizeof(expirations) || expirations == 0) {
        fail_value("timerfd read", amount < 0 ? amount : (long)expirations);
        goto done;
    }
    pass("timerfd + epoll");

done:
    close_if_open(epoll_fd);
    close_if_open(timer_fd);
}

static int event_name_equals(const struct t_inotify_event *event,
                             const char *name) {
    if (!event || !name || event->length == 0) return 0;
    return t_strncmp(event->name, name, event->length) == 0 ||
           t_strcmp(event->name, name) == 0;
}

static void test_inotify(void) {
    const char *first_path = "/tmp/glib-compat-a";
    const char *second_path = "/tmp/glib-compat-b";
    (void)t_unlink(first_path);
    (void)t_unlink(second_path);

    int notify_fd = t_inotify_init1(T_IN_NONBLOCK | T_IN_CLOEXEC);
    if (notify_fd < 0) {
        fail_value("inotify_init1", notify_fd);
        return;
    }
    uint32_t mask = T_IN_CREATE | T_IN_DELETE | T_IN_MOVED_FROM | T_IN_MOVED_TO;
    int watch = t_inotify_add_watch(notify_fd, "/tmp", mask);
    if (watch < 0) {
        fail_value("inotify_add_watch", watch);
        close_if_open(notify_fd);
        return;
    }

    int file = t_open(first_path, T_O_CREAT | T_O_WRONLY | T_O_TRUNC, 0644);
    if (file < 0) {
        fail_value("inotify create file", file);
        goto done;
    }
    long amount = t_write(file, "x", 1);
    close_if_open(file);
    if (amount != 1) {
        fail_value("inotify write file", amount);
        goto done;
    }
    int result = (int)t_syscall2(82, (long)first_path, (long)second_path);
    if (result < 0) {
        fail_value("inotify rename", result);
        goto done;
    }
    result = t_unlink(second_path);
    if (result < 0) {
        fail_value("inotify unlink", result);
        goto done;
    }

    uint8_t buffer[1024];
    amount = t_read(notify_fd, buffer, sizeof(buffer));
    if (amount <= 0) {
        fail_value("inotify read", amount);
        goto done;
    }
    int saw_create = 0;
    int saw_moved_from = 0;
    int saw_moved_to = 0;
    int saw_delete = 0;
    size_t offset = 0;
    while (offset + sizeof(struct t_inotify_event) <= (size_t)amount) {
        const struct t_inotify_event *event =
            (const struct t_inotify_event *)(const void *)(buffer + offset);
        size_t event_size = sizeof(*event) + event->length;
        if (event_size < sizeof(*event) || offset + event_size > (size_t)amount)
            break;
        if (event->wd == watch) {
            if ((event->mask & T_IN_CREATE) && event_name_equals(event, "glib-compat-a"))
                saw_create = 1;
            if ((event->mask & T_IN_MOVED_FROM) && event_name_equals(event, "glib-compat-a"))
                saw_moved_from = 1;
            if ((event->mask & T_IN_MOVED_TO) && event_name_equals(event, "glib-compat-b"))
                saw_moved_to = 1;
            if ((event->mask & T_IN_DELETE) && event_name_equals(event, "glib-compat-b"))
                saw_delete = 1;
        }
        offset += event_size;
    }
    if (saw_create && saw_moved_from && saw_moved_to && saw_delete)
        pass("inotify create/move/delete");
    else
        fail_value("inotify event set",
                   saw_create | (saw_moved_from << 1) | (saw_moved_to << 2) |
                   (saw_delete << 3));

done:
    (void)t_unlink(first_path);
    (void)t_unlink(second_path);
    (void)t_inotify_rm_watch(notify_fd, watch);
    close_if_open(notify_fd);
}

static struct t_cmsghdr *next_header(struct t_cmsghdr *header) {
    return (struct t_cmsghdr *)((uint8_t *)header + T_CMSG_ALIGN(header->length));
}

static void test_named_unix_socket(void) {
    int server = -1;
    int client = -1;
    int accepted = -1;
    struct t_sockaddr_un address;
    t_memset(&address, 0, sizeof(address));
    address.family = T_AF_UNIX;
    t_strcpy(address.path, "/tmp/glib-compat-bus");

    server = t_socket(T_AF_UNIX, T_SOCK_STREAM | T_SOCK_CLOEXEC, 0);
    if (server < 0) {
        fail_value("Unix listener socket", server);
        return;
    }
    int result = t_bind(server, &address, sizeof(address));
    if (result < 0) {
        fail_value("Unix pathname bind", result);
        goto done;
    }
    result = t_listen(server, 4);
    if (result < 0) {
        fail_value("Unix listen", result);
        goto done;
    }

    int option = 0;
    unsigned int option_length = sizeof(option);
    result = t_getsockopt(server, T_SOL_SOCKET, T_SO_ACCEPTCONN,
                          &option, &option_length);
    if (result < 0 || option != 1) {
        fail_value("SO_ACCEPTCONN", result < 0 ? result : option);
        goto done;
    }
    struct t_sockaddr_un local;
    unsigned int local_length = sizeof(local);
    t_memset(&local, 0, sizeof(local));
    result = t_getsockname(server, &local, &local_length);
    if (result < 0 || local.family != T_AF_UNIX ||
        t_strcmp(local.path, address.path) != 0) {
        fail_value("Unix getsockname", result);
        goto done;
    }

    client = t_socket(T_AF_UNIX,
                      T_SOCK_STREAM | T_SOCK_NONBLOCK | T_SOCK_CLOEXEC, 0);
    if (client < 0) {
        fail_value("Unix client socket", client);
        goto done;
    }
    result = t_connect(client, &address, sizeof(address));
    if (result < 0) {
        fail_value("Unix connect", result);
        goto done;
    }
    option = -1;
    option_length = sizeof(option);
    result = t_getsockopt(client, T_SOL_SOCKET, T_SO_ERROR,
                          &option, &option_length);
    if (result < 0 || option != 0) {
        fail_value("SO_ERROR", result < 0 ? result : option);
        goto done;
    }
    struct t_sockaddr_un peer;
    unsigned int peer_length = sizeof(peer);
    t_memset(&peer, 0, sizeof(peer));
    result = t_getpeername(client, &peer, &peer_length);
    if (result < 0 || peer.family != T_AF_UNIX ||
        t_strcmp(peer.path, address.path) != 0) {
        fail_value("Unix getpeername", result);
        goto done;
    }

    accepted = t_accept4(server, T_SOCK_NONBLOCK | T_SOCK_CLOEXEC);
    if (accepted < 0) {
        fail_value("accept4", accepted);
        goto done;
    }
    result = check_fd_flags(accepted, T_FD_CLOEXEC, T_O_NONBLOCK);
    if (result != 0) {
        fail_value("accept4 flags", result);
        goto done;
    }
    pass("pathname Unix socket + accept4");

done:
    close_if_open(accepted);
    close_if_open(client);
    close_if_open(server);
}

static void test_unix_credentials_and_rights(void) {
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    int received_fd = -1;
    int result = t_socketpair(T_AF_UNIX,
                              T_SOCK_STREAM | T_SOCK_CLOEXEC,
                              0, sockets);
    if (result < 0) {
        fail_value("socketpair", result);
        return;
    }
    result = t_pipe2(pipe_fds, T_O_CLOEXEC);
    if (result < 0) {
        fail_value("SCM_RIGHTS pipe", result);
        goto done;
    }

    struct t_ucred peer;
    unsigned int peer_length = sizeof(peer);
    t_memset(&peer, 0, sizeof(peer));
    result = t_getsockopt(sockets[0], T_SOL_SOCKET, T_SO_PEERCRED,
                          &peer, &peer_length);
    if (result < 0 || peer_length != sizeof(peer) || peer.pid != t_getpid()) {
        fail_value("SO_PEERCRED", result < 0 ? result : peer.pid);
        goto done;
    }
    int enabled = 1;
    result = t_setsockopt(sockets[1], T_SOL_SOCKET, T_SO_PASSCRED,
                          &enabled, sizeof(enabled));
    if (result < 0) {
        fail_value("SO_PASSCRED", result);
        goto done;
    }

    uint64_t send_control_words[8];
    t_memset(send_control_words, 0, sizeof(send_control_words));
    struct t_cmsghdr *send_header = (struct t_cmsghdr *)(void *)send_control_words;
    send_header->length = T_CMSG_LEN(sizeof(int));
    send_header->level = T_SOL_SOCKET;
    send_header->type = T_SCM_RIGHTS;
    *(int *)(void *)T_CMSG_DATA(send_header) = pipe_fds[0];
    char send_byte = 'F';
    struct t_iovec send_iov = {&send_byte, 1};
    struct t_msghdr send_message;
    t_memset(&send_message, 0, sizeof(send_message));
    send_message.iov = &send_iov;
    send_message.iov_length = 1;
    send_message.control = send_control_words;
    send_message.control_length = T_CMSG_SPACE(sizeof(int));
    long amount = t_sendmsg(sockets[0], &send_message, 0);
    if (amount != 1) {
        fail_value("sendmsg SCM_RIGHTS", amount);
        goto done;
    }

    uint64_t receive_control_words[16];
    t_memset(receive_control_words, 0, sizeof(receive_control_words));
    char receive_byte = 0;
    struct t_iovec receive_iov = {&receive_byte, 1};
    struct t_msghdr receive_message;
    t_memset(&receive_message, 0, sizeof(receive_message));
    receive_message.iov = &receive_iov;
    receive_message.iov_length = 1;
    receive_message.control = receive_control_words;
    receive_message.control_length = sizeof(receive_control_words);
    amount = t_recvmsg(sockets[1], &receive_message, T_MSG_CMSG_CLOEXEC);
    if (amount != 1 || receive_byte != 'F' ||
        (receive_message.flags & T_MSG_CTRUNC)) {
        fail_value("recvmsg SCM_RIGHTS", amount);
        goto done;
    }

    int saw_rights = 0;
    int saw_credentials = 0;
    size_t offset = 0;
    while (offset + sizeof(struct t_cmsghdr) <= receive_message.control_length) {
        struct t_cmsghdr *header =
            (struct t_cmsghdr *)(void *)((uint8_t *)receive_message.control + offset);
        if (header->length < sizeof(*header) ||
            offset + header->length > receive_message.control_length)
            break;
        if (header->level == T_SOL_SOCKET && header->type == T_SCM_RIGHTS &&
            header->length >= T_CMSG_LEN(sizeof(int))) {
            received_fd = *(int *)(void *)T_CMSG_DATA(header);
            saw_rights = received_fd >= 0;
        } else if (header->level == T_SOL_SOCKET &&
                   header->type == T_SCM_CREDENTIALS &&
                   header->length >= T_CMSG_LEN(sizeof(struct t_ucred))) {
            const struct t_ucred *credentials =
                (const struct t_ucred *)(const void *)T_CMSG_DATA(header);
            saw_credentials = credentials->pid == t_getpid();
        }
        struct t_cmsghdr *next = next_header(header);
        size_t next_offset = (size_t)((uint8_t *)next -
                                     (uint8_t *)receive_message.control);
        if (next_offset <= offset) break;
        offset = next_offset;
    }
    if (!saw_rights || !saw_credentials) {
        fail_value("SCM_RIGHTS/SCM_CREDENTIALS",
                   saw_rights | (saw_credentials << 1));
        goto done;
    }
    if (t_fcntl(received_fd, T_F_GETFD, 0) != T_FD_CLOEXEC) {
        fail_value("MSG_CMSG_CLOEXEC", received_fd);
        goto done;
    }

    close_if_open(pipe_fds[0]);
    pipe_fds[0] = -1;
    char pipe_byte = 'R';
    amount = t_write(pipe_fds[1], &pipe_byte, 1);
    if (amount != 1) {
        fail_value("SCM_RIGHTS write peer", amount);
        goto done;
    }
    char transferred_byte = 0;
    amount = t_read(received_fd, &transferred_byte, 1);
    if (amount != 1 || transferred_byte != pipe_byte) {
        fail_value("SCM_RIGHTS transferred fd", amount);
        goto done;
    }
    pass("Unix credentials + SCM_RIGHTS");

done:
    close_if_open(received_fd);
    close_if_open(pipe_fds[0]);
    close_if_open(pipe_fds[1]);
    close_if_open(sockets[0]);
    close_if_open(sockets[1]);
}

static void test_clock_nanosleep(void) {
    struct t_timespec request = {0, 2000000};
    struct t_timespec before;
    struct t_timespec after;
    int result = t_clock_gettime(&before);
    if (result < 0) {
        fail_value("clock_gettime before", result);
        return;
    }
    result = t_clock_nanosleep(CLOCK_MONOTONIC, 0, &request, NULL);
    if (result < 0) {
        fail_value("clock_nanosleep", result);
        return;
    }
    result = t_clock_gettime(&after);
    if (result < 0) {
        fail_value("clock_gettime after", result);
        return;
    }
    int64_t elapsed = (after.tv_sec - before.tv_sec) * 1000000000LL +
                      (after.tv_nsec - before.tv_nsec);
    if (elapsed >= 1000000LL) pass("clock_nanosleep");
    else fail_value("clock_nanosleep elapsed", (long)elapsed);
}

int main(int argc, char **argv) {
    if (argc == 2 && t_strcmp(argv[1], "--verify-cloexec") == 0)
        return t_fcntl(TEST_DUP_FD, T_F_GETFD, 0) == -9 ? 0 : 1;

    test_pipe2_dup3();
    test_fd_capacity();
    test_exec_cloexec();
    test_eventfd_epoll();
    test_timerfd_epoll();
    test_inotify();
    test_named_unix_socket();
    test_unix_credentials_and_rights();
    test_clock_nanosleep();

    if (failures == 0) {
        t_puts("GLIB-COMPAT: ALL TESTS PASSED\n");
        return 0;
    }
    t_puts("GLIB-COMPAT: FAILURES=");
    t_print_long(failures);
    t_puts("\n");
    return 1;
}
