#include <stddef.h>
#include <stdint.h>
#include "../include/devnum.h"
#include "../include/drm.h"

#include "../include/kstring.h"
#include "../include/net/netlink.h"
#include "../include/sound.h"
#include "../include/sysfs.h"
#include "../include/vfs.h"

/*
 * Just enough /sys for udev to find the devices this kernel has.
 *
 * Weston finds both its display and its input devices through libudev, which
 * finds them by walking /sys and reading what udevd wrote about each one.
 * Nothing here is a real sysfs: it is the handful of files udev actually
 * reads, laid out where Linux lays them out and derived once at boot from the
 * devices that already exist.
 *
 *   /sys/devices/<name>/uevent       properties on read, an action on write
 *   /sys/devices/<name>/subsystem    symlink whose basename is the subsystem
 *   /sys/class/<subsystem>/<name>    symlink an enumeration walks
 *   /sys/dev/char/<major>:<minor>    symlink a device number is looked up by
 *
 * The last two are what make udev report a sensible name: it resolves the link
 * and takes the last component, so `card0` rather than `226:0`.
 *
 * Writing an action into a uevent file is how a device is announced -- it is
 * what `udevadm trigger` does to every one of them at boot, and without an
 * answer udevd never learns that anything exists. See uevent_write().
 *
 * /sys/dev/block exists even with nothing in it, because a scan that cannot
 * open one of its directories gives up on the whole enumeration.
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
 * A published device, kept because a uevent has to be built again every time
 * one is asked for, and the file that triggers it only knows which device it
 * belongs to.
 *
 * The table is filled at boot and never grows: there is no hotplug here, so
 * the devices that exist are the ones this kernel found while starting.
 */
#define SYSFS_MAX_DEVICES 8

struct sysfs_device {
    char devpath[64];       /* /devices/<name>, which is udev's DEVPATH */
    char properties[256];   /* the uevent body, NUL-separated KEY=VALUE */
    size_t properties_length;
};

static struct sysfs_device sysfs_devices[SYSFS_MAX_DEVICES];
static size_t sysfs_device_count;
static uint32_t sysfs_sequence;

/*
 * Announce one device on the netlink group udevd listens to.
 *
 * The wire format is Linux's: a summary line of "<action>@<devpath>", then the
 * properties as NUL-terminated KEY=VALUE strings with ACTION and DEVPATH
 * repeated among them. udev reads the summary only to recognise the message as
 * one of the kernel's and takes everything it uses from the properties.
 */
static void uevent_send(const struct sysfs_device *device, const char *action) {
    char message[512];
    size_t used = 0;
    append_string(message, sizeof(message), &used, action);
    append_string(message, sizeof(message), &used, "@");
    append_string(message, sizeof(message), &used, device->devpath);
    if (used + 1 >= sizeof(message)) return;
    message[used++] = '\0';

    append_string(message, sizeof(message), &used, "ACTION=");
    append_string(message, sizeof(message), &used, action);
    if (used + 1 >= sizeof(message)) return;
    message[used++] = '\0';

    append_string(message, sizeof(message), &used, "DEVPATH=");
    append_string(message, sizeof(message), &used, device->devpath);
    if (used + 1 >= sizeof(message)) return;
    message[used++] = '\0';

    if (used + device->properties_length >= sizeof(message)) return;
    memcpy(message + used, device->properties, device->properties_length);
    used += device->properties_length;

    /* udevd keys its event queue on the sequence number and drops an event
       that arrives without one. */
    append_string(message, sizeof(message), &used, "SEQNUM=");
    append_number(message, sizeof(message), &used, ++sysfs_sequence);
    if (used + 1 >= sizeof(message)) return;
    message[used++] = '\0';

    netlink_uevent_broadcast(message, used);
}

/*
 * A write to a uevent file is a request to announce the device, not an edit.
 *
 * `udevadm trigger` walks /sys writing "add" into every one of these, and that
 * is the whole of cold-plug: nothing else tells udevd about a device that
 * existed before it started. Treating the write as data instead -- which is
 * what a file in this tree does by default -- overwrote the first four bytes
 * of the properties with "add" and announced nothing.
 */
static int64_t uevent_write(struct vfs_node *node, uint64_t offset, size_t size,
                            const void *buffer) {
    (void)offset;
    const struct sysfs_device *device = (const struct sysfs_device *)node->fs_private;
    if (!device || !size) return (int64_t)size;

    char action[16];
    size_t length = 0;
    const char *text = (const char *)buffer;
    while (length < size && length + 1 < sizeof(action) &&
           text[length] != '\n' && text[length] != '\0')
        length++;
    memcpy(action, text, length);
    action[length] = '\0';
    if (action[0]) uevent_send(device, action);
    /* The whole write is consumed either way: a caller given a short count for
       an action we did not recognise would keep trying to finish it. */
    return (int64_t)size;
}

/*
 * Register one device: its directory, its uevent, its subsystem link, and the
 * two symlinks that point back at it.
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
    struct vfs_node *uevent_node = vfs_create_file(file, uevent, length, 0, 1);

    /* Keep the properties, and point the file at them, so that a later write
       announces this device rather than scribbling on the file. */
    if (uevent_node && sysfs_device_count < SYSFS_MAX_DEVICES) {
        struct sysfs_device *device = &sysfs_devices[sysfs_device_count++];
        size_t at = 0;
        append_string(device->devpath, sizeof(device->devpath), &at, "/devices/");
        append_string(device->devpath, sizeof(device->devpath), &at, name);
        device->devpath[at] = '\0';
        /* The same KEY=VALUE lines the file holds, separated by NUL rather
           than by newline: that is the form the netlink message wants. */
        size_t out = 0;
        for (size_t index = 0; index < length && out + 1 < sizeof(device->properties);
             index++)
            device->properties[out++] =
                uevent[index] == '\n' ? 0 : uevent[index];
        device->properties_length = out;
        uevent_node->fs_private = device;
        uevent_node->write = uevent_write;
        uevent_node->mode = 0644;
    }

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

    /* The two ways back to the device directory, both two levels down from
       /sys and both written relative: libudev resolves a sysfs symlink by
       counting the leading "../" and cutting that many components off the
       link's own path, so an absolute target comes out as nonsense. */
    char target[64];
    used = 0;
    append_string(target, sizeof(target), &used, "../../devices/");
    append_string(target, sizeof(target), &used, name);
    target[used] = '\0';

    /* /sys/class/<subsystem>/<name>, which is what an enumeration walks. */
    used = 0;
    append_string(link, sizeof(link), &used, class_path);
    append_string(link, sizeof(link), &used, "/");
    append_string(link, sizeof(link), &used, name);
    link[used] = '\0';
    (void)vfs_create_symlink(link, target, 0);

    /* /sys/dev/char/<major>:<minor>, how a device number is looked back up. */
    char devnum[64];
    used = 0;
    append_string(devnum, sizeof(devnum), &used, "/sys/dev/char/");
    append_number(devnum, sizeof(devnum), &used, major);
    append_string(devnum, sizeof(devnum), &used, ":");
    append_number(devnum, sizeof(devnum), &used, minor);
    devnum[used] = '\0';
    (void)vfs_create_symlink(devnum, target, 0);
}

void sysfs_init(void) {
    struct vfs_node *sys = vfs_mkdir_p("/sys");
    if (!sys) return;
    vfs_mount_builtin("sysfs", "/sys", "sysfs", sys);
    if (!vfs_mkdir_p("/sys/devices")) return;
    if (!vfs_mkdir_p("/sys/class")) return;
    if (!vfs_mkdir_p("/sys/dev/char")) return;
    /* Empty, but it has to exist: libudev-zero fails the whole enumeration if
       either of its two scan directories cannot be opened. */
    if (!vfs_mkdir_p("/sys/dev/block")) return;

    if (drm_available())
        publish_device("card0", "dri/card0", "drm", NULL, DEV_MAJOR_DRM,
                       DEV_MINOR_DRM_CARD0);

    /* PipeWire enumerates sound cards through udev rather than by scanning
       /dev/snd, so a working card that is not published here is invisible. */
    if (sound_card_available()) {
        publish_device("controlC0", "snd/controlC0", "sound", NULL,
                       DEV_MAJOR_SOUND, DEV_MINOR_SOUND_CONTROL);
        publish_device("pcmC0D0p", "snd/pcmC0D0p", "sound", NULL,
                       DEV_MAJOR_SOUND, DEV_MINOR_SOUND_PCM_PLAYBACK);
    }

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
