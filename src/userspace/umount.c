/*
 * umount(8). The flags Linux takes say how hard to try when a filesystem is
 * busy; nothing here can be busy in that sense, so they are accepted and
 * ignored rather than refused.
 */
#include "tunix_libc.h"

int main(int argc, char **argv) {
    const char *target = NULL;
    for (int at = 1; at < argc; at++) {
        if (argv[at][0] == '-' && argv[at][1]) continue;
        if (target) {
            t_puterr("umount: too many arguments\n");
            return 1;
        }
        target = argv[at];
    }
    if (!target) {
        t_puterr("usage: umount <target>\n");
        return 1;
    }

    int status = t_umount2(target, 0);
    if (status < 0) {
        t_puterr("umount: ");
        t_puterr(status == -1 ? "that filesystem is part of the system\n" :
                 status == -22 ? "nothing is mounted there\n" :
                 "cannot unmount\n");
        return 1;
    }
    return 0;
}
