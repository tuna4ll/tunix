#include <stddef.h>
#include <stdint.h>
#include "tunix_libc.h"
#include <tunix/input_event.h>

#define MAX_INPUT_FDS 2U
#define EVENTS_PER_READ 16U

struct monitored_input {
    int fd;
    const char *path;
    struct tunix_input_device_info info;
};

static void print_number(long value) {
    t_print_long(value);
}

static const char *event_type_name(uint16_t type) {
    switch (type) {
        case TUNIX_EV_SYN: return "SYN";
        case TUNIX_EV_KEY: return "KEY";
        case TUNIX_EV_REL: return "REL";
        default: return "UNKNOWN";
    }
}

static const char *event_code_name(const struct tunix_input_event *event) {
    if (event->type == TUNIX_EV_SYN) {
        if (event->code == TUNIX_SYN_REPORT) return "REPORT";
        if (event->code == TUNIX_SYN_DROPPED) return "DROPPED";
    }
    if (event->type == TUNIX_EV_REL) {
        if (event->code == TUNIX_REL_X) return "X";
        if (event->code == TUNIX_REL_Y) return "Y";
        if (event->code == TUNIX_REL_WHEEL) return "WHEEL";
    }
    if (event->type == TUNIX_EV_KEY) {
        if (event->code == TUNIX_BTN_LEFT) return "BTN_LEFT";
        if (event->code == TUNIX_BTN_RIGHT) return "BTN_RIGHT";
        if (event->code == TUNIX_BTN_MIDDLE) return "BTN_MIDDLE";
        if (event->code == TUNIX_BTN_SIDE) return "BTN_SIDE";
        if (event->code == TUNIX_BTN_EXTRA) return "BTN_EXTRA";
    }
    return NULL;
}

static void print_device(const struct monitored_input *input) {
    t_puts("input-test: opened ");
    t_puts(input->path);
    t_puts(" (\"");
    t_puts(input->info.name);
    t_puts("\", device=");
    print_number((long)input->info.device_id);
    t_puts(", abi=");
    print_number((long)input->info.abi_version);
    t_puts(")\n");
}

static void print_event(const struct monitored_input *input,
                        const struct tunix_input_event *event) {
    t_puts(input->path);
    t_puts(": ");
    t_puts(event_type_name(event->type));
    t_puts(" ");
    const char *name = event_code_name(event);
    if (name) {
        t_puts(name);
    } else {
        t_puts("code=");
        print_number((long)event->code);
    }
    t_puts(" value=");
    print_number((long)event->value);
    t_puts(" time=");
    print_number((long)event->tv_sec);
    t_puts(".");
    print_number((long)event->tv_usec);
    t_puts("\n");
}

static int open_input(struct monitored_input *input, const char *path) {
    input->fd = t_open(path, T_O_RDONLY | T_O_NONBLOCK, 0);
    input->path = path;
    if (input->fd < 0) return -1;
    t_memset(&input->info, 0, sizeof(input->info));
    if (t_ioctl(input->fd, TUNIX_EVIOCGINFO, &input->info) < 0) {
        t_close(input->fd);
        input->fd = -1;
        return -1;
    }
    print_device(input);
    return 0;
}

/* --- evdev self-check ---------------------------------------------------- */

/*
 * The Linux evdev interface, checked without touching the hardware.
 *
 * This is what libinput does before it will look at a device at all, and it is
 * worth having separately: when weston says "not using input device" it does
 * not say which of a dozen probes disagreed with it.
 */

#define EVIOC_R(nr, size) (0x80000000UL | ((unsigned long)(size) << 16) | \
                           ((unsigned long)'E' << 8) | (unsigned long)(nr))
#define EVIOC_W(nr, size) (0x40000000UL | ((unsigned long)(size) << 16) | \
                           ((unsigned long)'E' << 8) | (unsigned long)(nr))

#define EVIOCGVERSION EVIOC_R(0x01, 4)
#define EVIOCGID EVIOC_R(0x02, 8)
#define EVIOCGNAME(len) EVIOC_R(0x06, len)
#define EVIOCGBIT(ev, len) EVIOC_R(0x20 + (ev), len)
#define EVIOCSCLOCKID EVIOC_W(0xa0, 4)
#define EVIOCGRAB EVIOC_W(0x90, 4)

static int evdev_failures;

static void evdev_check(int ok, const char *what) {
    t_puts(ok ? "evdev: ok   " : "evdev: FAIL ");
    t_puts(what);
    t_puts("\n");
    if (!ok) evdev_failures++;
}

static int bit_is_set(const unsigned char *bits, unsigned bit) {
    return (bits[bit / 8U] >> (bit % 8U)) & 1U;
}

static int evdev_selftest(const char *path, int expect_pointer) {
    int fd = t_open(path, T_O_RDWR | T_O_NONBLOCK, 0);
    t_puts("evdev: ");
    t_puts(path);
    t_puts("\n");
    if (fd < 0) {
        evdev_check(0, "open");
        return 1;
    }

    int32_t version = 0;
    evdev_check(t_ioctl(fd, EVIOCGVERSION, &version) == 0 && version != 0,
                "EVIOCGVERSION");

    uint16_t id[4] = { 0, 0, 0, 0 };
    evdev_check(t_ioctl(fd, EVIOCGID, id) == 0 && id[0] != 0,
                "EVIOCGID reports a bus type");

    char name[64];
    t_memset(name, 0, sizeof(name));
    evdev_check(t_ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && name[0],
                "EVIOCGNAME");
    t_puts("evdev:      name=");
    t_puts(name);
    t_puts("\n");

    unsigned char types[4];
    t_memset(types, 0, sizeof(types));
    evdev_check(t_ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) > 0 &&
                bit_is_set(types, TUNIX_EV_KEY),
                "EVIOCGBIT(0) reports EV_KEY");
    /* A keyboard that also claimed EV_REL would be taken for a pointer, so the
       absence matters as much as the presence. */
    evdev_check(bit_is_set(types, TUNIX_EV_REL) == (expect_pointer != 0),
                expect_pointer ? "EV_REL is present on the pointer"
                               : "EV_REL is absent on the keyboard");

    unsigned char keys[96];
    t_memset(keys, 0, sizeof(keys));
    evdev_check(t_ioctl(fd, EVIOCGBIT(TUNIX_EV_KEY, sizeof(keys)), keys) > 0,
                "EVIOCGBIT(EV_KEY)");
    if (expect_pointer) {
        evdev_check(bit_is_set(keys, TUNIX_BTN_LEFT), "the pointer has BTN_LEFT");
    } else {
        evdev_check(bit_is_set(keys, TUNIX_KEY_A) && bit_is_set(keys, TUNIX_KEY_ENTER),
                    "the keyboard has ordinary keys");
    }

    if (expect_pointer) {
        unsigned char rel[4];
        t_memset(rel, 0, sizeof(rel));
        evdev_check(t_ioctl(fd, EVIOCGBIT(TUNIX_EV_REL, sizeof(rel)), rel) > 0 &&
                    bit_is_set(rel, TUNIX_REL_X) && bit_is_set(rel, TUNIX_REL_Y),
                    "EVIOCGBIT(EV_REL) reports X and Y");
    }

    /* libinput switches every device to the monotonic clock immediately and
       gives up on one that will not. */
    int32_t monotonic = 1;
    evdev_check(t_ioctl(fd, EVIOCSCLOCKID, &monotonic) == 0,
                "EVIOCSCLOCKID(CLOCK_MONOTONIC)");

    evdev_check(t_ioctl(fd, EVIOCGRAB, (void *)1) == 0, "EVIOCGRAB");
    evdev_check(t_ioctl(fd, EVIOCGRAB, (void *)0) == 0, "EVIOCGRAB release");

    t_close(fd);
    return 0;
}

int main(int argc, char **argv) {
    struct monitored_input inputs[MAX_INPUT_FDS];
    struct t_pollfd pollfds[MAX_INPUT_FDS];
    unsigned count = 0;

    if (argc == 2 && argv[1][0] == '-') {
        /* --evdev: probe the Linux interface and exit, no input required. */
        evdev_selftest("/dev/input/event0", 0);
        evdev_selftest("/dev/input/event1", 1);
        if (evdev_failures) {
            t_puts("evdev: FAIL\n");
            return 1;
        }
        t_puts("evdev: PASS\n");
        return 0;
    }

    if (argc > 2) {
        t_puterr("usage: input-test [--evdev | /dev/input/eventN]\n");
        return 2;
    }

    if (argc == 2) {
        if (open_input(&inputs[count], argv[1]) != 0) {
            t_puterr("input-test: cannot open input device\n");
            return 1;
        }
        count++;
    } else {
        if (open_input(&inputs[count], "/dev/input/event0") == 0) count++;
        if (count < MAX_INPUT_FDS &&
            open_input(&inputs[count], "/dev/input/event1") == 0) count++;
    }

    if (!count) {
        t_puterr("input-test: no event devices found\n");
        return 1;
    }

    t_puts("input-test: move the mouse or press keys; Ctrl-C exits\n");
    for (unsigned i = 0; i < count; i++) {
        pollfds[i].fd = inputs[i].fd;
        pollfds[i].events = T_POLLIN;
        pollfds[i].revents = 0;
    }

    for (;;) {
        int ready = t_poll(pollfds, count, -1);
        if (ready == -T_EINTR) continue;
        if (ready < 0) {
            t_puterr("input-test: poll failed\n");
            return 1;
        }

        for (unsigned i = 0; i < count; i++) {
            if (!(pollfds[i].revents & T_POLLIN)) continue;
            for (;;) {
                struct tunix_input_event events[EVENTS_PER_READ];
                long amount = t_read(inputs[i].fd, events, sizeof(events));
                if (amount == -T_EAGAIN) break;
                if (amount < 0) {
                    t_puterr("input-test: read failed\n");
                    return 1;
                }
                if (amount == 0) break;
                if ((size_t)amount % sizeof(events[0]) != 0U) {
                    t_puterr("input-test: malformed event stream\n");
                    return 1;
                }
                size_t event_count = (size_t)amount / sizeof(events[0]);
                for (size_t event = 0; event < event_count; event++)
                    print_event(&inputs[i], &events[event]);
            }
        }
    }
}
