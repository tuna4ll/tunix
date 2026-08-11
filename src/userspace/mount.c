/*
 * mount(8), as much of it as this kernel has a filesystem layer for.
 *
 *   mount                            list what is mounted
 *   mount -t tmpfs tmpfs /somewhere  mount a fresh empty tree
 *   mount --bind /a /b               show one directory at a second place
 *   mount -o remount,ro /somewhere   change the flags of an existing mount
 *
 * With no arguments it prints /proc/mounts, which is where the kernel keeps
 * the same table this writes into.
 */
#include "tunix_libc.h"

#define MS_RDONLY 0x0001UL
#define MS_NOSUID 0x0002UL
#define MS_NODEV 0x0004UL
#define MS_NOEXEC 0x0008UL
#define MS_REMOUNT 0x0020UL
#define MS_BIND 0x1000UL

static void fail(const char *text) {
    t_puterr("mount: ");
    t_puterr(text);
    t_puterr("\n");
}

static int list_mounts(void) {
    int fd = t_open("/proc/mounts", T_O_RDONLY, 0);
    if (fd < 0) {
        fail("cannot open /proc/mounts");
        return 1;
    }
    char buffer[1024];
    for (;;) {
        long got = t_read(fd, buffer, sizeof(buffer));
        if (got <= 0) break;
        (void)t_write(1, buffer, (size_t)got);
    }
    t_close(fd);
    return 0;
}

/* One -o argument, which is a comma-separated list. Unknown names are an
   error rather than ignored: silently dropping "ro" would mount read-write. */
static int parse_options(const char *options, unsigned long *flags) {
    while (*options) {
        const char *start = options;
        while (*options && *options != ',') options++;
        size_t length = (size_t)(options - start);
        if (*options == ',') options++;
        if (!length) continue;

        struct { const char *name; unsigned long flag; } known[] = {
            {"ro", MS_RDONLY}, {"rw", 0}, {"nosuid", MS_NOSUID},
            {"nodev", MS_NODEV}, {"noexec", MS_NOEXEC},
            {"remount", MS_REMOUNT}, {"bind", MS_BIND}, {"defaults", 0},
        };
        int matched = 0;
        for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (t_strlen(known[i].name) != length) continue;
            int same = 1;
            for (size_t at = 0; at < length; at++)
                if (known[i].name[at] != start[at]) same = 0;
            if (!same) continue;
            *flags |= known[i].flag;
            matched = 1;
            break;
        }
        if (!matched) {
            fail("unknown option");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *type = NULL;
    const char *source = NULL;
    const char *target = NULL;
    unsigned long flags = 0;

    for (int at = 1; at < argc; at++) {
        const char *argument = argv[at];
        if (t_strcmp(argument, "-t") == 0 && at + 1 < argc) {
            type = argv[++at];
        } else if (t_strcmp(argument, "-o") == 0 && at + 1 < argc) {
            if (parse_options(argv[++at], &flags) != 0) return 1;
        } else if (t_strcmp(argument, "--bind") == 0) {
            flags |= MS_BIND;
        } else if (argument[0] == '-' && argument[1]) {
            fail("unknown argument");
            return 1;
        } else if (!source) {
            source = argument;
        } else if (!target) {
            target = argument;
        } else {
            fail("too many arguments");
            return 1;
        }
    }

    if (!source) return list_mounts();
    /* "mount -o remount,ro /somewhere" names only the target. */
    if (!target) {
        target = source;
        source = NULL;
    }
    if (!type) type = (flags & MS_BIND) ? "none" : "tmpfs";

    int status = t_mount(source, target, type, flags, 0);
    if (status < 0) {
        fail(status == -1 ? "operation not permitted" :
             status == -2 ? "no such file or directory" :
             status == -16 ? "target is busy" :
             status == -19 ? "no filesystem driver for that type" :
             status == -20 ? "target is not a directory" :
             "invalid argument");
        return 1;
    }
    return 0;
}
