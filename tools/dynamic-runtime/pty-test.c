/*
 * pty-test -- the pseudo-terminal sequence a terminal emulator actually uses.
 *
 * weston-terminal calls forkpty() and, when it fails, prints one strerror()
 * string for a sequence of five syscalls. This walks the same sequence a step
 * at a time so the failing one names itself.
 *
 *   open /dev/ptmx        the master
 *   TIOCSPTLCK            unlock the slave
 *   TIOCGPTN              which slave
 *   open /dev/pts/N       the slave
 *   TIOCSCTTY             make it the child's controlling terminal
 *
 * Then it runs the real forkpty() and echoes a line through the pair, which is
 * what an emulator does with every keystroke.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *what) {
    if (condition) {
        printf("pty-test: ok   %s\n", what);
    } else {
        printf("pty-test: FAIL %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures++;
    }
}

int main(void) {
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    check(master >= 0, "open /dev/ptmx");
    if (master < 0) return 1;

    int unlock = 0;
    check(ioctl(master, TIOCSPTLCK, &unlock) == 0, "TIOCSPTLCK unlocks the slave");

    int number = -1;
    check(ioctl(master, TIOCGPTN, &number) == 0 && number >= 0,
          "TIOCGPTN reports a slave number");

    char path[32];
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    int slave = open(path, O_RDWR | O_NOCTTY);
    check(slave >= 0, "open the slave");

    /* TIOCSCTTY needs a session of its own, so this half runs in a child that
       has just called setsid() -- exactly what login_tty() does inside
       forkpty(). Doing it here would steal the shell's controlling terminal. */
    if (slave >= 0) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            _exit(ioctl(slave, TIOCSCTTY, 0) == 0 ? 0 : 1);
        }
        int status = 1;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "TIOCSCTTY on the slave after setsid");
        close(slave);
    }
    close(master);

    /* The whole thing at once, the way a terminal emulator does it. */
    int pty = -1;
    pid_t child = forkpty(&pty, NULL, NULL, NULL);
    check(child >= 0, "forkpty");
    if (child == 0) {
        /* Prove the pair carries data in both directions. */
        char line[64];
        ssize_t got = read(0, line, sizeof(line));
        if (got > 0) (void)!write(1, line, (size_t)got);
        _exit(0);
    }
    if (child > 0) {
        static const char message[] = "tunix\n";
        check(write(pty, message, sizeof(message) - 1) == (ssize_t)sizeof(message) - 1,
              "write to the master");

        char echoed[128];
        size_t total = 0;
        /* The line comes back twice: once echoed by the line discipline, once
           written by the child. Either arriving is enough. */
        for (int attempt = 0; attempt < 200 && total < sizeof(echoed) - 1; attempt++) {
            ssize_t got = read(pty, echoed + total, sizeof(echoed) - 1 - total);
            if (got > 0) {
                total += (size_t)got;
                if (memchr(echoed, 'x', total)) break;
            } else {
                usleep(10000);
            }
        }
        echoed[total] = '\0';
        check(strstr(echoed, "tunix") != NULL, "the master reads the line back");

        int status = 0;
        waitpid(child, &status, 0);
        close(pty);
    }

    if (failures) {
        printf("pty-test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("pty-test: PASS\n");
    return 0;
}
