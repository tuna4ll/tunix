#include <stddef.h>
#include <stdint.h>
#include "tunix_libc.h"

/*
 * chvt: switch to a virtual terminal, or say which one is in front.
 *
 * The same thing Ctrl+Alt+F2 does, from a program rather than from the
 * keyboard -- which is how a display manager claims a terminal of its own, and
 * how a script can put the machine back on the desktop after using a console.
 *
 * The ioctls are asked of /dev/tty0, "whichever terminal is active", because
 * that is the one node that exists whatever the caller is standing on.
 */

#define VT_GETSTATE   0x5603UL
#define VT_ACTIVATE   0x5606UL
#define VT_WAITACTIVE 0x5607UL

struct vt_stat {
    uint16_t active;
    uint16_t signal;
    uint16_t state;
};

static void print_number(unsigned value) {
    char buffer[12];
    unsigned length = 0;
    if (!value) buffer[length++] = '0';
    while (value) {
        buffer[length++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (length) {
        char digit = buffer[--length];
        (void)t_write(1, &digit, 1);
    }
}

int main(int argc, char **argv) {
    int fd = t_open("/dev/tty0", T_O_RDWR, 0);
    if (fd < 0) {
        t_puterr("chvt: cannot open /dev/tty0\n");
        return 1;
    }

    if (argc < 2) {
        struct vt_stat state;
        if (t_ioctl(fd, VT_GETSTATE, &state) < 0) {
            t_puterr("chvt: cannot read the terminal state\n");
            return 1;
        }
        print_number(state.active);
        (void)t_write(1, "\n", 1);
        return 0;
    }

    unsigned target = 0;
    for (const char *walk = argv[1]; *walk; walk++) {
        if (*walk < '0' || *walk > '9') {
            t_puterr("usage: chvt [terminal]\n");
            return 1;
        }
        target = target * 10U + (unsigned)(*walk - '0');
    }
    if (!target) {
        t_puterr("chvt: terminals are numbered from 1\n");
        return 1;
    }

    if (t_ioctl(fd, VT_ACTIVATE, (void *)(uintptr_t)target) < 0) {
        t_puterr("chvt: cannot switch\n");
        return 1;
    }
    /* The switch may be waiting on whatever was on screen to release it, so
       the wait is what makes this command mean "it has happened". */
    if (t_ioctl(fd, VT_WAITACTIVE, (void *)(uintptr_t)target) < 0) {
        t_puterr("chvt: the switch did not complete\n");
        return 1;
    }
    return 0;
}
