#include "tunix_libc.h"

static int spawn(char *const argv[]) {
    long child = t_fork();
    if (child < 0) return -1;
    if (child == 0) {
        t_setpgid(0, 0);
        int pgid = (int)t_getpgrp();
        t_ioctl(0, T_TIOCSPGRP, &pgid);
        t_execve(argv[0], argv, t_environ);
        t_puterr("init: cannot execute /bin/bash\n");
        t_exit(127);
    }

    int status = 0;
    while (t_waitpid(child, &status, 0) < 0) t_yield();
    return (status >> 8) & 0xff;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    t_mkdir("/tmp", 0777);
    t_mkdir("/home", 0755);

    char *shell[] = {"/bin/bash", "--login", "-i", 0};
    for (;;) {
        int status = spawn(shell);
        if (status < 0) {
            t_puterr("init: failed to start GNU Bash\n");
            return 1;
        }
        t_sleep_ms(500);
    }
}
