#include <stddef.h>
#include <stdint.h>
#include "../include/devnum.h"
#include "../include/drm.h"
#include "../include/pci.h"
#include "../include/virtgpu.h"

#include "../include/kstring.h"
#include "../include/net/netlink.h"
#include "../include/sound.h"
#include "../include/sysfs.h"
#include "../include/vfs.h"

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

#define SYSFS_MAX_DEVICES 8

struct sysfs_device {
    char devpath[64];
    char properties[256];
    size_t properties_length;
};

static struct sysfs_device sysfs_devices[SYSFS_MAX_DEVICES];
static size_t sysfs_device_count;
static uint32_t sysfs_sequence;

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

    append_string(message, sizeof(message), &used, "SEQNUM=");
    append_number(message, sizeof(message), &used, ++sysfs_sequence);
    if (used + 1 >= sizeof(message)) return;
    message[used++] = '\0';

    netlink_uevent_broadcast(message, used);
}

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
    return (int64_t)size;
}

static void append_hex(char *out, size_t limit, size_t *used, uint32_t value,
                       unsigned digits) {
    static const char alphabet[] = "0123456789abcdef";
    while (digits--) {
        if (*used + 1 >= limit) return;
        out[(*used)++] = alphabet[(value >> (digits * 4)) & 0xFU];
    }
}

static void publish_hex_attribute(const char *directory, const char *name,
                                  uint32_t value, unsigned digits) {
    char path[224];
    size_t used = 0;
    append_string(path, sizeof(path), &used, directory);
    append_string(path, sizeof(path), &used, "/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';

    char text[16];
    size_t length = 0;
    append_string(text, sizeof(text), &length, "0x");
    append_hex(text, sizeof(text), &length, value, digits);
    append_string(text, sizeof(text), &length, "\n");
    (void)vfs_create_file(path, text, length, 0, 1);
}

static void append_hex64(char *out, size_t limit, size_t *used, uint64_t value) {
    append_string(out, limit, used, "0x");
    append_hex(out, limit, used, (uint32_t)(value >> 32), 8);
    append_hex(out, limit, used, (uint32_t)value, 8);
}

static void publish_pci_device(const struct pci_device *device, void *context) {
    (void)context;
    char directory[96];
    size_t used = 0;
    append_string(directory, sizeof(directory), &used, "/sys/bus/pci/devices/0000:");
    append_hex(directory, sizeof(directory), &used, device->bus, 2);
    append_string(directory, sizeof(directory), &used, ":");
    append_hex(directory, sizeof(directory), &used, device->slot, 2);
    append_string(directory, sizeof(directory), &used, ".");
    append_hex(directory, sizeof(directory), &used, device->function, 1);
    directory[used] = '\0';
    if (!vfs_mkdir_p(directory)) return;

    uint8_t config[256];
    for (unsigned offset = 0; offset < sizeof(config); offset += 4) {
        uint32_t word = pci_config_read32(device->bus, device->slot,
                                          device->function, (uint8_t)offset);
        config[offset + 0] = (uint8_t)word;
        config[offset + 1] = (uint8_t)(word >> 8);
        config[offset + 2] = (uint8_t)(word >> 16);
        config[offset + 3] = (uint8_t)(word >> 24);
    }

    char file[128];
    used = 0;
    append_string(file, sizeof(file), &used, directory);
    append_string(file, sizeof(file), &used, "/config");
    file[used] = '\0';
    (void)vfs_create_file(file, config, sizeof(config), 0, 1);

    publish_hex_attribute(directory, "vendor", device->vendor_id, 4);
    publish_hex_attribute(directory, "device", device->device_id, 4);
    publish_hex_attribute(directory, "class",
                          ((uint32_t)device->class_code << 16) |
                          ((uint32_t)device->subclass << 8) | device->prog_if, 6);

    char text[16];
    size_t length = 0;
    append_number(text, sizeof(text), &length, device->irq_line);
    append_string(text, sizeof(text), &length, "\n");
    used = 0;
    append_string(file, sizeof(file), &used, directory);
    append_string(file, sizeof(file), &used, "/irq");
    file[used] = '\0';
    (void)vfs_create_file(file, text, length, 0, 1);

    char resource[512];
    length = 0;
    for (unsigned index = 0; index < 7; index++) {
        uint64_t start = index < 6 ? pci_bar_address(device, index) : 0;
        append_hex64(resource, sizeof(resource), &length, start);
        append_string(resource, sizeof(resource), &length, " ");
        append_hex64(resource, sizeof(resource), &length, start);
        append_string(resource, sizeof(resource), &length, " ");
        append_hex64(resource, sizeof(resource), &length, 0);
        append_string(resource, sizeof(resource), &length, "\n");
    }
    used = 0;
    append_string(file, sizeof(file), &used, directory);
    append_string(file, sizeof(file), &used, "/resource");
    file[used] = '\0';
    (void)vfs_create_file(file, resource, length, 0, 1);
}

static void publish_pci_bus(void) {
    if (!vfs_mkdir_p("/sys/bus/pci/devices")) return;
    pci_for_each_device(publish_pci_device, NULL);
}

static void publish_pci_parent(const char *name) {
    struct virtgpu_pci_identity id;
    if (virtgpu_pci_identity(&id) != 0) return;

    char path[192];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/");
    append_string(path, sizeof(path), &used, name);
    append_string(path, sizeof(path), &used, "/device");
    path[used] = '\0';
    if (!vfs_mkdir_p(path)) return;

    char file[224];
    (void)vfs_mkdir_p("/sys/bus/pci");
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/subsystem");
    file[used] = '\0';
    (void)vfs_create_symlink(file, "/sys/bus/pci", 0);

    char uevent[192];
    size_t length = 0;
    append_string(uevent, sizeof(uevent), &length, "DRIVER=virtio-pci\nPCI_ID=");
    append_hex(uevent, sizeof(uevent), &length, id.vendor, 4);
    append_string(uevent, sizeof(uevent), &length, ":");
    append_hex(uevent, sizeof(uevent), &length, id.device, 4);
    append_string(uevent, sizeof(uevent), &length, "\nPCI_SLOT_NAME=0000:");
    append_hex(uevent, sizeof(uevent), &length, id.bus, 2);
    append_string(uevent, sizeof(uevent), &length, ":");
    append_hex(uevent, sizeof(uevent), &length, id.slot, 2);
    append_string(uevent, sizeof(uevent), &length, ".");
    append_hex(uevent, sizeof(uevent), &length, id.function, 1);
    append_string(uevent, sizeof(uevent), &length, "\n");
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/uevent");
    file[used] = '\0';
    (void)vfs_create_file(file, uevent, length, 0, 1);

    uint8_t config[64];
    for (unsigned offset = 0; offset < sizeof(config); offset += 4) {
        uint32_t word = pci_config_read32(id.bus, id.slot, id.function,
                                          (uint8_t)offset);
        config[offset + 0] = (uint8_t)word;
        config[offset + 1] = (uint8_t)(word >> 8);
        config[offset + 2] = (uint8_t)(word >> 16);
        config[offset + 3] = (uint8_t)(word >> 24);
    }
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/config");
    file[used] = '\0';
    (void)vfs_create_file(file, config, sizeof(config), 0, 1);

    publish_hex_attribute(path, "vendor",
                          (uint32_t)(config[0] | (config[1] << 8)), 4);
    publish_hex_attribute(path, "device",
                          (uint32_t)(config[2] | (config[3] << 8)), 4);
    publish_hex_attribute(path, "revision", config[8], 2);
    publish_hex_attribute(path, "subsystem_vendor",
                          (uint32_t)(config[44] | (config[45] << 8)), 4);
    publish_hex_attribute(path, "subsystem_device",
                          (uint32_t)(config[46] | (config[47] << 8)), 4);
}

static void publish_drm_nodes(const char *name) {
    static const char *const nodes[] = { "card0", "renderD128" };
    for (unsigned index = 0; index < 2; index++) {
        char path[192];
        size_t used = 0;
        append_string(path, sizeof(path), &used, "/sys/devices/");
        append_string(path, sizeof(path), &used, name);
        append_string(path, sizeof(path), &used, "/device/drm/");
        append_string(path, sizeof(path), &used, nodes[index]);
        path[used] = '\0';
        (void)vfs_mkdir_p(path);
    }
}

static void publish_device(const char *name, const char *devname,
                           const char *subsystem, const char *extra,
                           uint32_t major, uint32_t minor) {
    char path[128];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';
    if (!vfs_mkdir_p(path)) return;

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
    if (extra) append_string(uevent, sizeof(uevent), &length, extra);

    char file[160];
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/uevent");
    file[used] = '\0';
    struct vfs_node *uevent_node = vfs_create_file(file, uevent, length, 0, 1);

    if (uevent_node && sysfs_device_count < SYSFS_MAX_DEVICES) {
        struct sysfs_device *device = &sysfs_devices[sysfs_device_count++];
        size_t at = 0;
        append_string(device->devpath, sizeof(device->devpath), &at, "/devices/");
        append_string(device->devpath, sizeof(device->devpath), &at, name);
        device->devpath[at] = '\0';
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

    char target[64];
    used = 0;
    append_string(target, sizeof(target), &used, "../../devices/");
    append_string(target, sizeof(target), &used, name);
    target[used] = '\0';

    used = 0;
    append_string(link, sizeof(link), &used, class_path);
    append_string(link, sizeof(link), &used, "/");
    append_string(link, sizeof(link), &used, name);
    link[used] = '\0';
    (void)vfs_create_symlink(link, target, 0);

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
    if (!vfs_mkdir_p("/sys/dev/block")) return;

    publish_pci_bus();

    if (drm_available()) {
        int rendering = virtgpu_virgl_available();
        publish_device("card0", "dri/card0", "drm",
                       rendering ? "DRIVER=virtio_gpu\nDEVTYPE=drm_minor\n"
                                 : "DRIVER=tunixdrm\nDEVTYPE=drm_minor\n",
                       DEV_MAJOR_DRM, DEV_MINOR_DRM_CARD0);
        if (rendering) {
            publish_device("renderD128", "dri/renderD128", "drm",
                           "DRIVER=virtio_gpu\nDEVTYPE=drm_render_minor\n",
                           DEV_MAJOR_DRM, DEV_MINOR_DRM_RENDER0);
            publish_drm_nodes("card0");
            publish_drm_nodes("renderD128");
            publish_pci_parent("card0");
            publish_pci_parent("renderD128");
        }
    }

    if (sound_card_available()) {
        publish_device("controlC0", "snd/controlC0", "sound", NULL,
                       DEV_MAJOR_SOUND, DEV_MINOR_SOUND_CONTROL);
        publish_device("pcmC0D0p", "snd/pcmC0D0p", "sound", NULL,
                       DEV_MAJOR_SOUND, DEV_MINOR_SOUND_PCM_PLAYBACK);
    }

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

        const char *tags = device == 0U
            ? "ID_INPUT=1\nID_INPUT_KEYBOARD=1\n"
            : "ID_INPUT=1\nID_INPUT_MOUSE=1\n";

        publish_device(name, devname, "input", tags, DEV_MAJOR_INPUT,
                       DEV_MINOR_INPUT_EVENT_BASE + device);
    }
}
