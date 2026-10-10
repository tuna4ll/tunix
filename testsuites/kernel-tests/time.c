#define TEST_NAME "time"
#include "test.h"

static void clocks(void) {
    struct timespec first, second, wall;
    expect_eq(clock_gettime(CLOCK_MONOTONIC, &first), 0, "the monotonic clock reads");
    int forward = 1;
    for (int i = 0; i < 1000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &second);
        forward &= elapsed_ms(&first, &second) >= 0 && second.nsec < 1000000000;
        first = second;
    }
    expect(forward, "and never runs backwards");
    clock_gettime(CLOCK_REALTIME, &wall);
    expect(wall.sec > 1577836800, "the wall clock is past 2020");
}

static void sleeping(void) {
    s64 start = now_ms();
    sleep_ms(50);
    s64 slept = now_ms() - start;
    expect(slept >= 50 && slept < 1000, "nanosleep(50 ms) sleeps 50 ms, not much more");
}

static void eventfds(void) {
    int fd = (int)eventfd(0, EFD_NONBLOCK);
    u64 value = 3;
    write(fd, &value, 8);
    value = 4;
    write(fd, &value, 8);
    value = 0;
    expect_eq(read(fd, &value, 8), 8, "an eventfd reads eight bytes");
    expect_eq((s64)value, 7, "holding the sum of what was written");
    expect_eq(read(fd, &value, 8), -EAGAIN, "and is empty afterwards");
    close(fd);

    fd = (int)eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    u64 a = 0, b = 0;
    read(fd, &a, 8);
    read(fd, &b, 8);
    expect(a == 1 && b == 1, "a semaphore eventfd counts down by one");
    expect_eq(read(fd, &a, 8), -EAGAIN, "until it reaches zero");
    close(fd);
}

static void epoll(void) {
    int epoll = (int)epoll_create();
    int event = (int)eventfd(0, EFD_NONBLOCK);
    expect(epoll >= 0, "epoll_create1 makes an instance");
    expect_eq(epoll_add(epoll, event, EPOLLIN, 77), 0, "an eventfd is added to it");
    struct epoll_event ready[4];
    expect_eq(epoll_wait(epoll, ready, 4, 0), 0, "nothing is ready yet");

    s64 start = now_ms();
    expect_eq(epoll_wait(epoll, ready, 4, 100), 0, "a 100 ms wait times out");
    expect(now_ms() - start >= 100, "after at least 100 ms");

    u64 one = 1;
    write(event, &one, 8);
    expect_eq(epoll_wait(epoll, ready, 4, 1000), 1, "a write makes the eventfd ready");
    expect(ready[0].events & EPOLLIN && ready[0].data == 77, "with the data it was added with");

    int timer = (int)timerfd_create(CLOCK_MONOTONIC);
    struct itimerspec period = {{0, 30000000}, {0, 30000000}};
    expect_eq(timerfd_settime(timer, &period), 0, "a 30 ms periodic timerfd is armed");
    epoll_add(epoll, timer, EPOLLIN, 88);
    read(event, &one, 8);

    start = now_ms();
    int ticks = 0;
    while (ticks < 5 && now_ms() - start < 2000) {
        if (epoll_wait(epoll, ready, 4, 1000) == 1 && ready[0].data == 88) {
            u64 expirations = 0;
            read(timer, &expirations, 8);
            ticks += (int)expirations;
        }
    }
    s64 took = now_ms() - start;
    expect(ticks >= 5, "epoll wakes for five timer expirations");
    expect(took >= 140 && took < 1000, "spread over about 150 ms");
    close(timer);
    close(event);
    close(epoll);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    clocks();
    sleeping();
    eventfds();
    epoll();
}
