#include <stddef.h>
#include <stdint.h>
#include "tunix_libc.h"

/*
 * getty: open a virtual terminal and hand it to login(1).
 *
 * Small as it is, this is what makes a terminal a *login* terminal rather than
 * a file that happens to print. Three things have to be true before login can
 * ask for a password, and nothing else does them:
 *
 *   - the process is a session leader, so the terminal it claims becomes the
 *     controlling terminal of a session of its own and not of whatever started
 *     it. Four gettys sharing one session would be four logins fighting over
 *     one foreground process group.
 *   - the terminal is on the standard descriptors, because that is where login
 *     and the shell after it look.
 *   - TIOCSCTTY, which is the claim itself: from then on Ctrl-C typed at this
 *     terminal reaches this session and no other.
 *
 * The service manager restarts it when the session ends, which is what brings
 * the login prompt back after a logout.
 */

#define TTY_NAME_MAX 64

static void write_all(const char *text) {
    size_t length = t_strlen(text);
    size_t written = 0;
    while (written < length) {
        long result = t_write(1, text + written, length - written);
        if (result <= 0) return;
        written += (size_t)result;
    }
}

/* "/dev/tty3" -> "tty3", for the banner. */
static const char *terminal_name(const char *path) {
    const char *name = path;
    for (const char *walk = path; *walk; walk++)
        if (*walk == '/') name = walk + 1;
    return name;
}

int main(int argc, char **argv) {
    const char *terminal = argc > 1 ? argv[1] : "/dev/tty1";

    /*
     * A process that is already a process group leader cannot start a session,
     * and a service manager has usually made it one. Forking gives a process
     * that is not, and the parent stays alive so the manager still has
     * something to watch and to restart.
     */
    if (t_setsid() < 0) {
        long child = t_fork();
        if (child > 0) {
            int status = 0;
            (void)t_waitpid(child, &status, 0);
            t_exit(0);
        }
        if (child < 0) {
            t_puterr("getty: cannot fork\n");
            return 1;
        }
        (void)t_setsid();
    }

    /* O_NOCTTY is deliberate: the claim below is explicit, so that a failure to
       make this terminal the controlling one is visible here rather than
       silently depending on how open() behaves for a session leader. */
    int fd = t_open(terminal, T_O_RDWR | T_O_NOCTTY, 0);
    if (fd < 0) {
        t_puterr("getty: cannot open ");
        t_puterr(terminal);
        t_puterr("\n");
        return 1;
    }

    if (t_ioctl(fd, T_TIOCSCTTY, NULL) < 0) {
        t_puterr("getty: cannot claim ");
        t_puterr(terminal);
        t_puterr("\n");
        return 1;
    }

    (void)t_dup2(fd, 0);
    (void)t_dup2(fd, 1);
    (void)t_dup2(fd, 2);
    if (fd > 2) (void)t_close(fd);

    write_all("\nTunix on ");
    write_all(terminal_name(terminal));
    write_all("\n\n");

    char *arguments[] = {(char *)"login", NULL};
    (void)t_execve("/bin/login", arguments, t_environ);
    t_puterr("getty: cannot run /bin/login\n");
    return 1;
}
