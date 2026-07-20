/*
 * kill-blocked-test -- kill a process that is asleep inside a blocking syscall.
 *
 * This is the smallest reproducer for the panic the Wayland roundtrip test hit:
 * a client blocked in a read, SIGKILLed by its parent, took the whole kernel
 * down with a supervisor-mode page fault at RIP 0x1.
 *
 * Tunix implements blocking syscalls by *rewinding* them -- the saved frame's
 * %rip is moved back over the syscall instruction and %rax reloaded with the
 * syscall number, so the process re-issues it when it next runs. A signal
 * arriving while a frame is in that state has to be handled carefully, which is
 * exactly the seam being tested here.
 *
 * Each case blocks a child a different way, because the wait paths differ:
 * a pipe read parks on a wait channel, poll parks on a deadline, and wait4 does
 * not rewind at all.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *what) {
    if (condition) {
        printf("kill-blocked-test: ok   %s\n", what);
    } else {
        printf("kill-blocked-test: FAIL %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures++;
    }
}

/* Kills a child that blocked the given way and reports whether it died of the
   signal rather than anything else. */
static void kill_while_blocked(const char *what, void (*block_forever)(int *pipe_fds)) {
    int fds[2];
    if (pipe(fds) != 0) {
        check(0, what);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        check(0, what);
        return;
    }
    if (pid == 0) {
        block_forever(fds);
        _exit(99); /* must never be reached */
    }

    /* Give the child time to actually reach the blocking call; killing it
       before it gets there would test nothing. */
    usleep(300000);
    kill(pid, SIGKILL);

    int status = 0;
    int reaped = waitpid(pid, &status, 0) == pid;
    close(fds[0]);
    close(fds[1]);

    check(reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, what);
}

static void block_in_read(int *fds) {
    char byte;
    close(fds[1]);
    (void)read(fds[0], &byte, 1);
}

static void block_in_poll(int *fds) {
    struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
    close(fds[1]);
    (void)poll(&pfd, 1, -1);
}

static void block_in_wait(int *fds) {
    (void)fds;
    /* wait() only blocks if there is actually something to wait for, so give
       it a grandchild that outlives the test. Without one it returns ECHILD
       immediately and nothing is being tested. */
    /* Long enough to still be running when the kill lands 300ms in, short
       enough that the orphan it becomes does not outlive the test. */
    if (fork() == 0) {
        sleep(5);
        _exit(0);
    }
    (void)wait(NULL);
}

int main(void) {
    /* Repeat: a frame-handling bug can depend on which process the scheduler
       happens to switch to, so a single pass can pass by luck. */
    for (int round = 0; round < 3; round++) {
        kill_while_blocked("SIGKILL a child blocked in read()", block_in_read);
        kill_while_blocked("SIGKILL a child blocked in poll()", block_in_poll);
        kill_while_blocked("SIGKILL a child blocked in wait()", block_in_wait);
    }

    if (failures) {
        printf("kill-blocked-test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("kill-blocked-test: PASS\n");
    return 0;
}
