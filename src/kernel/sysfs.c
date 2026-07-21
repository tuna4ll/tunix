#include <stddef.h>
#include <stdint.h>
#include "include/devnum.h"
#include "include/drm.h"

#include "include/kstring.h"
#include "include/sysfs.h"
#include "include/vfs.h"

/*
 * Just enough /sys for udev to enumerate devices.
 *
 * Weston's DRM backend finds both its display and its input devices through
 * libudev, and libudev-zero -- the small replacement we ship instead of
 * systemd's udev -- does that by scanning /sys/dev/char and /sys/dev/block.
 * Without those directories nothing is discoverable and the backend gives up
 * before it starts.
 *
 * This is not sysfs. It is the three things libudev-zero actually reads,
 * derived once at boot from the devices we already have:
 *
 *   /sys/devices/<name>/uevent       MAJOR=/MINOR=/DEVNAME= lines
 *   /sys/devices/<name>/subsystem    symlink whose basename is the subsystem
 *   /sys/dev/char/<major>:<minor>    symlink to the device directory
 *
 * The symlink is what makes udev report a sensible name: it resolves the
 * numeric path and takes the last component, so `card0` rather than `226:0`.
 *
 * /sys/dev/block exists even with nothing in it, because libudev-zero treats a
 * failed scandir of *either* directory as a failure of the whole enumeration.
 */

static void append_string(char *out, size_t limit, size_t *used, const char *text) {
    while (*text && *used + 1 < limit) out[(*used)++] = *text++;
}

static void append_number(char *out, size_t limit, size_t *used, uint32_t value) {
    char digits[12];
    int count = 0;
    if (!value) digits[count++] = '0';
    while (value && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count-- > 0 && *used + 1 < limit) out[(*used)++] = digits[count];
}

/*
 * Register one device: its directory, its uevent, its subsystem link, and the
 * /sys/dev/char entry that points back at it.
 */
static void publish_device(const char *name, const char *devname,
                           const char *subsystem, const char *extra,
                           uint32_t major, uint32_t minor) {
    char path[128];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';
    if (!vfs_mkdir_p(path)) return;

    /* uevent: the properties udev hands to its callers. DEVNAME is how a
       consumer gets from the sysfs entry back to /dev -- libudev-zero simply
       prefixes it with "/dev/", so it is a path relative to /dev and not
       always the same as the sysfs name: card0 lives at dri/card0. */
    char uevent[256];
    size_t length = 0;
    append_string(uevent, sizeof(uevent), &length, "MAJOR=");
    append_number(uevent, sizeof(uevent), &length, major);
    append_string(uevent, sizeof(uevent), &length, "\nMINOR=");
    append_number(uevent, sizeof(uevent), &length, minor);
    append_string(uevent, sizeof(uevent), &length, "\nDEVNAME=");
    append_string(uevent, sizeof(uevent), &length, devname);
    append_string(uevent, sizeof(uevent), &length, "\nSUBSYSTEM=");
    append_string(uevent, sizeof(uevent), &length, subsystem);
    append_string(uevent, sizeof(uevent), &length, "\n");
    /* Whatever else a consumer needs tagged. On Linux these come from udev's
       input_id builtin, which classifies a device by probing its evdev bits;
       there is nothing to probe here, so the answer is stated. libinput
       refuses any device that is not tagged, however well it works. */
    if (extra) append_string(uevent, sizeof(uevent), &length, extra);

    char file[160];
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/uevent");
    file[used] = '\0';
    (void)vfs_create_file(file, uevent, length, 0, 1);

    /* subsystem is read as a symlink and only its basename is used. */
    char class_path[128];
    used = 0;
    append_string(class_path, sizeof(class_path), &used, "/sys/class/");
    append_string(class_path, sizeof(class_path), &used, subsystem);
    class_path[used] = '\0';
    (void)vfs_mkdir_p(class_path);

    char link[160];
    used = 0;
    append_string(link, sizeof(link), &used, path);
    append_string(link, sizeof(link), &used, "/subsystem");
    link[used] = '\0';
    (void)vfs_create_symlink(link, class_path, 0);

    /* /sys/dev/char/<major>:<minor> -> the device directory. */
    char devnum[64];
    used = 0;
    append_string(devnum, sizeof(devnum), &used, "/sys/dev/char/");
    append_number(devnum, sizeof(devnum), &used, major);
    append_string(devnum, sizeof(devnum), &used, ":");
    append_number(devnum, sizeof(devnum), &used, minor);
    devnum[used] = '\0';
    (void)vfs_create_symlink(devnum, path, 0);
}

void sysfs_init(void) {
    if (!vfs_mkdir_p("/sys/devices")) return;
    if (!vfs_mkdir_p("/sys/class")) return;
    if (!vfs_mkdir_p("/sys/dev/char")) return;
    /* Empty, but it has to exist: libudev-zero fails the whole enumeration if
       either of its two scan directories cannot be opened. */
    if (!vfs_mkdir_p("/sys/dev/block")) return;

    if (drm_available())
        publish_device("card0", "dri/card0", "drm", NULL, DEV_MAJOR_DRM,
                       DEV_MINOR_DRM_CARD0);

    /* devfs attaches a fixed pair of evdev nodes, event0 for the keyboard and
       event1 for the mouse; this mirrors that rather than inventing an
       enumeration the input layer does not offer. Keep the two in step.
       libinput opens them by the DEVNAME each uevent carries. */
    for (unsigned device = 0; device < 2U; device++) {
        char name[32];
        size_t used = 0;
        append_string(name, sizeof(name), &used, "event");
        append_number(name, sizeof(name), &used, device);
        name[used] = '\0';
        char devname[40];
        size_t devname_used = 0;
        append_string(devname, sizeof(devname), &devname_used, "input/");
        append_string(devname, sizeof(devname), &devname_used, name);
        devname[devname_used] = '\0';

        /* event0 is the keyboard, event1 the mouse -- devfs attaches them in
           that order, and libinput wants each one told which it is. */
        const char *tags = device == 0U
            ? "ID_INPUT=1\nID_INPUT_KEYBOARD=1\n"
            : "ID_INPUT=1\nID_INPUT_MOUSE=1\n";

        publish_device(name, devname, "input", tags, DEV_MAJOR_INPUT,
                       DEV_MINOR_INPUT_EVENT_BASE + device);
    }
}
