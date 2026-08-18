#include <stddef.h>
#include "tunix_libc.h"

/*
 * void-login: stand-in for /bin/login on the void_distro experiment.
 *
 * getty always execs "/bin/login" (see getty.c) and hands it a session,
 * a controlling terminal, and descriptors 0/1/2 already pointed at it. The
 * real question the experiment asks is whether Void's own dynamically linked
 * userland -- bash, coreutils, its musl loader -- runs on this kernel at all,
 * which has nothing to do with whether Tunix's shadow/PAM stack understands
 * Void's /etc/shadow. So this skips authentication entirely and drops
 * straight into a root shell; it is a debug shim, not a login program, and
 * has no business existing outside that experiment.
 *
 * The environment is built here rather than inherited from getty/dinit,
 * because nothing upstream of this process has any reason to have set one.
 */

int main(void) {
    char *argv[] = {(char *)"-bash", NULL};
    char *envp[] = {
        (char *)"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        (char *)"HOME=/root",
        (char *)"TERM=linux",
        (char *)"SHELL=/bin/bash",
        (char *)"USER=root",
        (char *)"LOGNAME=root",
        NULL,
    };
    (void)t_execve("/bin/bash", argv, envp);
    t_puterr("void-login: cannot run /bin/bash\n");
    return 1;
}
