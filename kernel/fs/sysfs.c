#include <stddef.h>
#include <stdint.h>
#include <tunix/backlight.h>
#include <tunix/devnum.h>
#include <tunix/drm.h>
#include <tunix/pci.h>
#include <tunix/virtgpu.h>

#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/module.h>
#include <tunix/net/netlink.h>
#include <tunix/sound.h>
#include <tunix/sysfs.h>
#include <tunix/cpufreq.h>
#include <tunix/thermal.h>
#include <tunix/vt.h>
#include <tunix/vfs.h>

static void vfs_guard_release(int *unused) {
    (void)unused;
    vfs_lock_release();
}

#define VFS_GUARD \
    __attribute__((cleanup(vfs_guard_release))) int vfs_guard = (vfs_lock_acquire(), 0)

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

struct sysfs_device {
    char devpath[64];
    char properties[256];
    size_t properties_length;
};

static struct sysfs_device **sysfs_devices;
static size_t sysfs_device_count;
static size_t sysfs_device_capacity;
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
    VFS_GUARD;
    (void)offset;
    const struct sysfs_device *device = (const struct sysfs_device *)node->fs_private;
    if (!device || !size) return (int64_t)size;

    char action[16];
    size_t length = 0;
    const char *text = (const char *)buffer;
    while (length < size && length + 1 < sizeof(action) && text[length] != '\n' &&
           text[length] != '\0')
        length++;
    memcpy(action, text, length);
    action[length] = '\0';
    if (action[0]) uevent_send(device, action);
    return (int64_t)size;
}

static void append_hex(char *out, size_t limit, size_t *used, uint32_t value, unsigned digits) {
    static const char alphabet[] = "0123456789abcdef";
    while (digits--) {
        if (*used + 1 >= limit) return;
        out[(*used)++] = alphabet[(value >> (digits * 4)) & 0xFU];
    }
}

static void publish_hex_attribute(const char *directory, const char *name, uint32_t value,
                                  unsigned digits) {
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

static void pci_slot_name(char *out, size_t limit, size_t *used, const struct pci_device *device) {
    append_string(out, limit, used, "0000:");
    append_hex(out, limit, used, device->bus, 2);
    append_string(out, limit, used, ":");
    append_hex(out, limit, used, device->slot, 2);
    append_string(out, limit, used, ".");
    append_hex(out, limit, used, device->function, 1);
}

static void pci_device_path(char *out, size_t limit, const struct pci_device *device,
                            const char *suffix) {
    size_t used = 0;
    append_string(out, limit, &used, "/sys/devices/pci0000:00/");
    pci_slot_name(out, limit, &used, device);
    if (suffix) append_string(out, limit, &used, suffix);
    out[used] = '\0';
}

static struct sysfs_device *register_uevent(const char *devpath, const char *file,
                                            const char *properties, size_t length) {
    struct vfs_node *node = vfs_create_file(file, properties, length, 0, 1);
    if (!node) return NULL;
    if (sysfs_device_count == sysfs_device_capacity) {
        size_t capacity = sysfs_device_capacity ? sysfs_device_capacity * 2 : 32;
        struct sysfs_device **grown = kmalloc(capacity * sizeof(*grown));
        if (!grown) return NULL;
        if (sysfs_device_count) memcpy(grown, sysfs_devices, sysfs_device_count * sizeof(*grown));
        kfree(sysfs_devices);
        sysfs_devices = grown;
        sysfs_device_capacity = capacity;
    }
    struct sysfs_device *device = kmalloc(sizeof(*device));
    if (!device) return NULL;
    memset(device, 0, sizeof(*device));
    sysfs_devices[sysfs_device_count++] = device;
    size_t at = 0;
    append_string(device->devpath, sizeof(device->devpath), &at, devpath);
    device->devpath[at] = '\0';
    size_t out = 0;
    for (size_t index = 0; index < length && out + 1 < sizeof(device->properties); index++)
        device->properties[out++] = properties[index] == '\n' ? 0 : properties[index];
    device->properties_length = out;
    node->fs_private = device;
    node->write = uevent_write;
    node->mode = 0644;
    return device;
}

static void publish_pci_device(const struct pci_device *device, void *context) {
    (void)context;
    char directory[96];
    pci_device_path(directory, sizeof(directory), device, NULL);
    if (!vfs_mkdir_p(directory)) return;

    uint8_t config[256];
    for (unsigned offset = 0; offset < sizeof(config); offset += 4) {
        uint32_t word =
            pci_config_read32(device->bus, device->slot, device->function, (uint8_t)offset);
        config[offset + 0] = (uint8_t)word;
        config[offset + 1] = (uint8_t)(word >> 8);
        config[offset + 2] = (uint8_t)(word >> 16);
        config[offset + 3] = (uint8_t)(word >> 24);
    }

    char file[128];
    size_t used = 0;
    append_string(file, sizeof(file), &used, directory);
    append_string(file, sizeof(file), &used, "/config");
    file[used] = '\0';
    (void)vfs_create_file(file, config, sizeof(config), 0, 1);

    publish_hex_attribute(directory, "vendor", device->vendor_id, 4);
    publish_hex_attribute(directory, "device", device->device_id, 4);
    publish_hex_attribute(directory, "revision", config[8], 2);
    publish_hex_attribute(directory, "subsystem_vendor", (uint32_t)(config[44] | (config[45] << 8)),
                          4);
    publish_hex_attribute(directory, "subsystem_device", (uint32_t)(config[46] | (config[47] << 8)),
                          4);
    publish_hex_attribute(directory, "class",
                          ((uint32_t)device->class_code << 16) | ((uint32_t)device->subclass << 8) |
                              device->prog_if,
                          6);

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

    char alias[80];
    pci_modalias(device, alias, sizeof(alias));
    used = 0;
    append_string(file, sizeof(file), &used, directory);
    append_string(file, sizeof(file), &used, "/modalias");
    file[used] = '\0';
    char alias_file[96];
    size_t alias_file_length = 0;
    append_string(alias_file, sizeof(alias_file), &alias_file_length, alias);
    append_string(alias_file, sizeof(alias_file), &alias_file_length, "\n");
    (void)vfs_create_file(file, alias_file, alias_file_length, 0, 1);

    char slot[16];
    size_t slot_length = 0;
    pci_slot_name(slot, sizeof(slot), &slot_length, device);
    slot[slot_length] = '\0';

    char devpath[80];
    size_t devpath_length = 0;
    append_string(devpath, sizeof(devpath), &devpath_length, "/devices/pci0000:00/");
    append_string(devpath, sizeof(devpath), &devpath_length, slot);
    devpath[devpath_length] = '\0';

    char properties[256];
    length = 0;
    append_string(properties, sizeof(properties), &length, "DRIVER=\nPCI_CLASS=");
    append_hex(properties, sizeof(properties), &length,
               ((uint32_t)device->class_code << 16) | ((uint32_t)device->subclass << 8) |
                   device->prog_if,
               6);
    append_string(properties, sizeof(properties), &length, "\nPCI_ID=");
    append_hex(properties, sizeof(properties), &length, device->vendor_id, 4);
    append_string(properties, sizeof(properties), &length, ":");
    append_hex(properties, sizeof(properties), &length, device->device_id, 4);
    append_string(properties, sizeof(properties), &length, "\nPCI_SLOT_NAME=");
    append_string(properties, sizeof(properties), &length, slot);
    append_string(properties, sizeof(properties), &length, "\nSUBSYSTEM=pci\nMODALIAS=");
    append_string(properties, sizeof(properties), &length, alias);
    append_string(properties, sizeof(properties), &length, "\n");

    used = 0;
    append_string(file, sizeof(file), &used, directory);
    append_string(file, sizeof(file), &used, "/uevent");
    file[used] = '\0';
    (void)register_uevent(devpath, file, properties, length);

    used = 0;
    append_string(file, sizeof(file), &used, directory);
    append_string(file, sizeof(file), &used, "/subsystem");
    file[used] = '\0';
    (void)vfs_create_symlink(file, "/sys/bus/pci", 0);

    char link[96];
    used = 0;
    append_string(link, sizeof(link), &used, "/sys/bus/pci/devices/");
    append_string(link, sizeof(link), &used, slot);
    link[used] = '\0';
    char target[80];
    used = 0;
    append_string(target, sizeof(target), &used, "../../../devices/pci0000:00/");
    append_string(target, sizeof(target), &used, slot);
    target[used] = '\0';
    (void)vfs_create_symlink(link, target, 0);
}

static void driver_directory(char *out, size_t limit, const char *driver, const char *suffix) {
    size_t used = 0;
    append_string(out, limit, &used, "/sys/bus/pci/drivers/");
    append_string(out, limit, &used, driver);
    if (suffix) append_string(out, limit, &used, suffix);
    out[used] = '\0';
}

void sysfs_pci_driver_added(const char *driver) {
    VFS_GUARD;
    char path[96];
    driver_directory(path, sizeof(path), driver, NULL);
    struct vfs_node *node = vfs_mkdir_p(path);
    if (node) node->mode = 0555;
}

void sysfs_pci_driver_removed(const char *driver) {
    VFS_GUARD;
    char path[96];
    driver_directory(path, sizeof(path), driver, NULL);
    struct vfs_node *node = vfs_lookup(path);
    if (node && node->parent) (void)vfs_detach_child(node->parent, node);
}

void sysfs_pci_bound(const struct pci_device *device, const char *driver) {
    VFS_GUARD;
    char slot[16];
    size_t slot_length = 0;
    pci_slot_name(slot, sizeof(slot), &slot_length, device);
    slot[slot_length] = '\0';

    char link[128];
    pci_device_path(link, sizeof(link), device, "/driver");
    char target[96];
    size_t used = 0;
    append_string(target, sizeof(target), &used, "../../../bus/pci/drivers/");
    append_string(target, sizeof(target), &used, driver);
    target[used] = '\0';
    (void)vfs_create_symlink(link, target, 0);

    char reverse[128];
    used = 0;
    append_string(reverse, sizeof(reverse), &used, "/sys/bus/pci/drivers/");
    append_string(reverse, sizeof(reverse), &used, driver);
    append_string(reverse, sizeof(reverse), &used, "/");
    append_string(reverse, sizeof(reverse), &used, slot);
    reverse[used] = '\0';
    used = 0;
    char device_target[96];
    append_string(device_target, sizeof(device_target), &used, "../../../../devices/pci0000:00/");
    append_string(device_target, sizeof(device_target), &used, slot);
    device_target[used] = '\0';
    (void)vfs_create_symlink(reverse, device_target, 0);

    char devpath[80];
    used = 0;
    append_string(devpath, sizeof(devpath), &used, "/devices/pci0000:00/");
    append_string(devpath, sizeof(devpath), &used, slot);
    devpath[used] = '\0';
    for (size_t index = 0; index < sysfs_device_count; index++) {
        struct sysfs_device *entry = sysfs_devices[index];
        if (strcmp(entry->devpath, devpath) != 0) continue;
        size_t at = 0;
        append_string(entry->properties, sizeof(entry->properties), &at, "DRIVER=");
        append_string(entry->properties, sizeof(entry->properties), &at, driver);
        entry->properties[at] = '\0';
        break;
    }
}

void sysfs_pci_unbound(const struct pci_device *device) {
    VFS_GUARD;
    char link[128];
    pci_device_path(link, sizeof(link), device, "/driver");
    struct vfs_node *node = vfs_lookup_nofollow(link);
    if (node && node->parent) (void)vfs_detach_child(node->parent, node);
}

static void publish_pci_bus(void) {
    if (!vfs_mkdir_p("/sys/bus/pci/devices")) return;
    if (!vfs_mkdir_p("/sys/bus/pci/drivers")) return;
    if (!vfs_mkdir_p("/sys/devices/pci0000:00")) return;
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
        uint32_t word = pci_config_read32(id.bus, id.slot, id.function, (uint8_t)offset);
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

    publish_hex_attribute(path, "vendor", (uint32_t)(config[0] | (config[1] << 8)), 4);
    publish_hex_attribute(path, "device", (uint32_t)(config[2] | (config[3] << 8)), 4);
    publish_hex_attribute(path, "revision", config[8], 2);
    publish_hex_attribute(path, "subsystem_vendor", (uint32_t)(config[44] | (config[45] << 8)), 4);
    publish_hex_attribute(path, "subsystem_device", (uint32_t)(config[46] | (config[47] << 8)), 4);
}

static void publish_platform_parent(const char *name) {
    char path[192];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/");
    append_string(path, sizeof(path), &used, name);
    append_string(path, sizeof(path), &used, "/device");
    path[used] = '\0';
    if (!vfs_mkdir_p(path)) return;

    char file[224];
    (void)vfs_mkdir_p("/sys/bus/platform");
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/subsystem");
    file[used] = '\0';
    (void)vfs_create_symlink(file, "/sys/bus/platform", 0);

    static const char uevent[] =
        "DRIVER=simple-framebuffer\nMODALIAS=platform:simple-framebuffer\n";
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/uevent");
    file[used] = '\0';
    (void)vfs_create_file(file, uevent, sizeof(uevent) - 1, 0, 1);
}

static void publish_drm_nodes(const char *name, unsigned count) {
    static const char *const nodes[] = {"card0", "renderD128"};
    for (unsigned index = 0; index < count; index++) {
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

static void publish_device(const char *name, const char *devname, const char *subsystem,
                           const char *extra, uint32_t major, uint32_t minor) {
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

    char devpath[96];
    size_t devpath_length = 0;
    append_string(devpath, sizeof(devpath), &devpath_length, "/devices/");
    append_string(devpath, sizeof(devpath), &devpath_length, name);
    devpath[devpath_length] = '\0';
    (void)register_uevent(devpath, file, uevent, length);

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

    char target[128];
    used = 0;
    append_string(target, sizeof(target), &used, "../../devices/");
    append_string(target, sizeof(target), &used, name);
    target[used] = '\0';

    const char *base = name;
    for (const char *at = name; *at; at++)
        if (*at == '/') base = at + 1;
    used = 0;
    append_string(link, sizeof(link), &used, class_path);
    append_string(link, sizeof(link), &used, "/");
    append_string(link, sizeof(link), &used, base);
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

static int64_t module_section_read(struct vfs_node *node, uint64_t offset, size_t size,
                                   void *output);

static int64_t attribute_reply(const char *text, size_t length, uint64_t offset, size_t size,
                               void *output) {
    if (offset >= length) return 0;
    size_t available = length - (size_t)offset;
    if (size > available) size = available;
    memcpy(output, text + offset, size);
    return (int64_t)size;
}

static int64_t module_number_read(struct vfs_node *node, uint64_t offset, size_t size,
                                  void *output) {
    const struct module *module = (const struct module *)node->fs_private;
    if (!module) return 0;
    char text[24];
    size_t length = 0;
    uint64_t value = node->inode == 1 ? module->refs : module->bytes;
    append_number(text, sizeof(text), &length, (uint32_t)value);
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

static int64_t module_state_read(struct vfs_node *node, uint64_t offset, size_t size,
                                 void *output) {
    const struct module *module = (const struct module *)node->fs_private;
    if (!module) return 0;
    const char *state = module->state == MODULE_STATE_LIVE ? "live\n" : "coming\n";
    return attribute_reply(state, strlen(state), offset, size, output);
}

static int64_t module_parameter_read(struct vfs_node *node, uint64_t offset, size_t size,
                                     void *output) {
    const struct module *module = (const struct module *)node->fs_private;
    if (!module) return 0;
    char text[64];
    int length = module_param_format(module, (unsigned)node->inode, text, sizeof(text));
    if (length < 0) return 0;
    return attribute_reply(text, (size_t)length, offset, size, output);
}

static int64_t module_parameter_write(struct vfs_node *node, uint64_t offset, size_t size,
                                      const void *buffer) {
    (void)offset;
    struct module *module = (struct module *)node->fs_private;
    if (!module) return -1;
    int status = module_param_set(module, (unsigned)node->inode, (const char *)buffer, size);
    return status == 0 ? (int64_t)size : (int64_t)status;
}

static struct vfs_node *module_attribute(struct vfs_node *parent, const char *name,
                                         vfs_read_fn reader, void *context, uint64_t tag) {
    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE);
    if (!node) return NULL;
    node->mode = 0444;
    node->length = 64;
    node->read = reader;
    node->fs_private = context;
    node->inode = tag;
    if (vfs_attach(parent, node) != 0) return NULL;
    return node;
}

#define HWMON_INPUT 0
#define HWMON_CRIT  1
#define HWMON_LABEL 2

static int64_t hwmon_read(struct vfs_node *node, uint64_t offset, size_t size, void *output) {
    unsigned cpu = (unsigned)(node->inode & 0xFFFFU);
    unsigned kind = (unsigned)(node->inode >> 16);
    char text[32];
    size_t length = 0;
    if (kind == HWMON_LABEL) {
        append_string(text, sizeof(text), &length, "Core ");
        append_number(text, sizeof(text), &length, cpu);
    } else if (kind == HWMON_CRIT) {
        append_number(text, sizeof(text), &length, (uint32_t)thermal_state()->tjmax * 1000U);
    } else {
        struct thermal_reading reading;
        if (thermal_read(cpu, &reading) != 0 || reading.celsius < 0) return -5;
        append_number(text, sizeof(text), &length, (uint32_t)reading.celsius * 1000U);
    }
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

static void hwmon_attribute(struct vfs_node *directory, unsigned cpu, unsigned kind,
                            const char *suffix) {
    char name[32];
    size_t length = 0;
    append_string(name, sizeof(name), &length, "temp");
    append_number(name, sizeof(name), &length, cpu + 1U);
    append_string(name, sizeof(name), &length, suffix);
    name[length] = '\0';
    (void)module_attribute(directory, name, hwmon_read, NULL, ((uint64_t)kind << 16) | cpu);
}

static void publish_thermal(unsigned cpus) {
    if (thermal_supported() <= 0 || !cpus) return;
    struct vfs_node *directory = vfs_mkdir_p("/sys/devices/platform/coretemp.0/hwmon/hwmon0");
    if (!directory) return;
    (void)vfs_create_file("/sys/devices/platform/coretemp.0/hwmon/hwmon0/name", "coretemp\n", 9, 0,
                          1);
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        hwmon_attribute(directory, cpu, HWMON_INPUT, "_input");
        hwmon_attribute(directory, cpu, HWMON_CRIT, "_crit");
        hwmon_attribute(directory, cpu, HWMON_LABEL, "_label");
    }
    if (vfs_mkdir_p("/sys/class/hwmon"))
        (void)vfs_create_symlink("/sys/class/hwmon/hwmon0",
                                 "../../devices/platform/coretemp.0/hwmon/hwmon0", 0);
}

#define CPUFREQ_CURRENT 0
#define CPUFREQ_MIN     1
#define CPUFREQ_MAX     2
#define CPUFREQ_DRIVER  3

static int64_t cpufreq_attribute_read(struct vfs_node *node, uint64_t offset, size_t size,
                                      void *output) {
    unsigned cpu = (unsigned)(node->inode & 0xFFFFU);
    unsigned kind = (unsigned)(node->inode >> 16);
    const struct cpufreq_state *state = cpufreq_state();
    char text[32];
    size_t length = 0;
    if (kind == CPUFREQ_DRIVER) {
        append_string(text, sizeof(text), &length, "tunix-pstate");
    } else if (kind == CPUFREQ_MIN || kind == CPUFREQ_MAX) {
        uint32_t ratio = kind == CPUFREQ_MIN ? state->min_ratio : state->max_ratio;
        append_number(text, sizeof(text), &length, (uint32_t)(state->ratio_khz * ratio));
    } else {
        struct cpufreq_reading reading;
        if (cpufreq_read(cpu, &reading) != 0) return -5;
        uint64_t khz =
            reading.effective_khz ? reading.effective_khz : state->ratio_khz * reading.ratio;
        append_number(text, sizeof(text), &length, (uint32_t)khz);
    }
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

static void publish_cpu_list(const char *name, unsigned cpus) {
    char path[64];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/system/cpu/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';
    char text[24];
    size_t length = 0;
    append_string(text, sizeof(text), &length, "0");
    if (cpus > 1U) {
        append_string(text, sizeof(text), &length, "-");
        append_number(text, sizeof(text), &length, cpus - 1U);
    }
    append_string(text, sizeof(text), &length, "\n");
    (void)vfs_create_file(path, text, length, 0, 1);
}

static void publish_cpufreq(unsigned cpus) {
    static const char *const names[] = {"scaling_cur_freq", "cpuinfo_min_freq", "cpuinfo_max_freq",
                                        "scaling_driver"};
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        char path[80];
        size_t used = 0;
        append_string(path, sizeof(path), &used, "/sys/devices/system/cpu/cpu");
        append_number(path, sizeof(path), &used, cpu);
        append_string(path, sizeof(path), &used, "/cpufreq");
        path[used] = '\0';
        struct vfs_node *directory = vfs_mkdir_p(path);
        if (!directory) return;
        for (unsigned kind = 0; kind < 4U; kind++)
            (void)module_attribute(directory, names[kind], cpufreq_attribute_read, NULL,
                                   ((uint64_t)kind << 16) | cpu);
    }
}

static int64_t smi_count_read(struct vfs_node *node, uint64_t offset, size_t size, void *output) {
    (void)node;
    char text[24];
    size_t length = 0;
    append_number(text, sizeof(text), &length, (uint32_t)cpufreq_state()->smi_count);
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

void sysfs_publish_cpus(unsigned cpus) {
    if (!cpus || !vfs_mkdir_p("/sys/devices/system/cpu")) return;
    publish_cpu_list("online", cpus);
    publish_cpu_list("possible", cpus);
    publish_cpu_list("present", cpus);
    if (cpufreq_supported() > 0) {
        publish_cpufreq(cpus);
        struct vfs_node *system = vfs_mkdir_p("/sys/devices/system/cpu");
        if (system && cpufreq_state()->smi_counted)
            (void)module_attribute(system, "smi_count", smi_count_read, NULL, 0);
    }
    publish_thermal(cpus);
}

static void holders_refresh(struct vfs_node *directory) {
    static int busy;
    const struct module *owner = (const struct module *)directory->fs_private;
    if (!owner || busy) return;
    busy = 1;
    while (directory->children) (void)vfs_detach_child(directory, directory->children);
    for (struct module *user = module_list(); user; user = user->next) {
        for (unsigned index = 0; index < user->use_count; index++) {
            if (user->uses[index] != owner) continue;
            char target[MODULE_NAME_MAX + 8];
            size_t used = 0;
            append_string(target, sizeof(target), &used, "../../");
            append_string(target, sizeof(target), &used, user->name);
            target[used] = '\0';
            (void)vfs_attach_symlink(directory, user->name, target);
        }
    }
    busy = 0;
}

static struct vfs_node *module_directory(const char *name, const char *child) {
    char path[96];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/module/");
    append_string(path, sizeof(path), &used, name);
    if (child) {
        append_string(path, sizeof(path), &used, "/");
        append_string(path, sizeof(path), &used, child);
    }
    path[used] = '\0';
    return vfs_mkdir_p(path);
}

void sysfs_module_added(struct module *module) {
    VFS_GUARD;
    if (!module) return;
    struct vfs_node *root = module_directory(module->name, NULL);
    if (!root) return;
    root->mode = 0555;
    (void)module_attribute(root, "initstate", module_state_read, module, 0);
    (void)module_attribute(root, "refcnt", module_number_read, module, 1);
    (void)module_attribute(root, "coresize", module_number_read, module, 2);

    struct vfs_node *holders = module_directory(module->name, "holders");
    if (holders) {
        holders->mode = 0555;
        holders->fs_private = module;
        holders->refresh = holders_refresh;
    }

    struct vfs_node *sections = module_directory(module->name, "sections");
    if (sections) {
        sections->mode = 0555;
        (void)module_attribute(sections, ".text", module_section_read, module, 0);
        (void)module_attribute(sections, ".rodata", module_section_read, module, 1);
        (void)module_attribute(sections, ".data", module_section_read, module, 2);
    }

    if (!module->param_count) return;
    struct vfs_node *parameters = module_directory(module->name, "parameters");
    if (!parameters) return;
    parameters->mode = 0555;
    for (unsigned index = 0; index < module->param_count; index++) {
        struct vfs_node *node = module_attribute(parameters, module->params[index].name,
                                                 module_parameter_read, module, index);
        if (!node || module->params[index].type == MODULE_PARAM_STRING) continue;
        node->mode = module->params[index].mode;
        node->write = module_parameter_write;
    }
}

static int64_t module_section_read(struct vfs_node *node, uint64_t offset, size_t size,
                                   void *output) {
    const struct module *module = (const struct module *)node->fs_private;
    if (!module) return 0;
    uint64_t address = node->inode == 1 ? module->rodata
        : node->inode == 2              ? module->data
                                        : module->text;
    char text[32];
    size_t length = 0;
    append_hex64(text, sizeof(text), &length, address);
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

void sysfs_module_removed(const char *name) {
    VFS_GUARD;
    char path[96];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/module/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';
    struct vfs_node *node = vfs_lookup(path);
    if (node && node->parent) (void)vfs_detach_child(node->parent, node);
}

static void remove_published(const char *name) {
    char path[128];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';
    struct vfs_node *node = vfs_lookup(path);
    if (node && node->parent) (void)vfs_detach_child(node->parent, node);

    for (size_t index = 0; index < sysfs_device_count; index++) {
        if (strcmp(sysfs_devices[index]->devpath + 9, name) != 0) continue;
        uevent_send(sysfs_devices[index], "remove");
        sysfs_devices[index]->devpath[0] = '\0';
        break;
    }
}

static void announce(const char *name) {
    for (size_t index = 0; index < sysfs_device_count; index++) {
        if (strcmp(sysfs_devices[index]->devpath + 9, name) != 0) continue;
        uevent_send(sysfs_devices[index], "add");
        return;
    }
}

void sysfs_publish_sound(void) {
    VFS_GUARD;
    publish_device("controlC0", "snd/controlC0", "sound", NULL, DEV_MAJOR_SOUND,
                   DEV_MINOR_SOUND_CONTROL);
    publish_device("pcmC0D0p", "snd/pcmC0D0p", "sound", NULL, DEV_MAJOR_SOUND,
                   DEV_MINOR_SOUND_PCM_PLAYBACK);
    announce("controlC0");
    announce("pcmC0D0p");
}

void sysfs_remove_sound(void) {
    VFS_GUARD;
    remove_published("controlC0");
    remove_published("pcmC0D0p");
}

static void publish_input_parent(const char *name, const char *label) {
    char path[128];
    size_t used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';
    if (!vfs_mkdir_p(path)) return;

    char uevent[160];
    size_t length = 0;
    append_string(uevent, sizeof(uevent), &length, "NAME=\"");
    append_string(uevent, sizeof(uevent), &length, label);
    append_string(uevent, sizeof(uevent), &length, "\"\nSUBSYSTEM=input\n");
    char file[160];
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/uevent");
    file[used] = '\0';
    char devpath[128];
    size_t devpath_used = 0;
    append_string(devpath, sizeof(devpath), &devpath_used, "/devices/");
    append_string(devpath, sizeof(devpath), &devpath_used, name);
    devpath[devpath_used] = '\0';
    (void)register_uevent(devpath, file, uevent, length);

    char text[64];
    length = 0;
    append_string(text, sizeof(text), &length, label);
    append_string(text, sizeof(text), &length, "\n");
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/name");
    file[used] = '\0';
    (void)vfs_create_file(file, text, length, 0, 1);

    (void)vfs_mkdir_p("/sys/class/input");
    char link[160];
    used = 0;
    append_string(link, sizeof(link), &used, path);
    append_string(link, sizeof(link), &used, "/subsystem");
    link[used] = '\0';
    (void)vfs_create_symlink(link, "/sys/class/input", 0);

    const char *base = name;
    for (const char *at = name; *at; at++)
        if (*at == '/') base = at + 1;
    char target[128];
    used = 0;
    append_string(target, sizeof(target), &used, "../../devices/");
    append_string(target, sizeof(target), &used, name);
    target[used] = '\0';
    used = 0;
    append_string(link, sizeof(link), &used, "/sys/class/input/");
    append_string(link, sizeof(link), &used, base);
    link[used] = '\0';
    (void)vfs_create_symlink(link, target, 0);
}

#define BACKLIGHT_EINVAL 22
#define BACKLIGHT_EIO    5

enum backlight_attribute {
    BACKLIGHT_BRIGHTNESS,
    BACKLIGHT_ACTUAL,
    BACKLIGHT_MAX,
    BACKLIGHT_TYPE,
};

static const struct backlight_device *backlight;
static uint32_t backlight_level;

static int64_t backlight_read(struct vfs_node *node, uint64_t offset, size_t size, void *output) {
    char text[32];
    size_t length = 0;
    if (!backlight) return 0;
    switch (node->inode) {
    case BACKLIGHT_BRIGHTNESS: append_number(text, sizeof(text), &length, backlight_level); break;
    case BACKLIGHT_ACTUAL:     append_number(text, sizeof(text), &length, backlight->get()); break;
    case BACKLIGHT_MAX:
        append_number(text, sizeof(text), &length, backlight->max_brightness);
        break;
    default: append_string(text, sizeof(text), &length, backlight->type); break;
    }
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

static int64_t backlight_write(struct vfs_node *node, uint64_t offset, size_t size,
                               const void *buffer) {
    (void)offset;
    if (!backlight || node->inode != BACKLIGHT_BRIGHTNESS) return -BACKLIGHT_EINVAL;
    const char *text = (const char *)buffer;
    size_t index = 0;
    while (index < size && text[index] == ' ') index++;
    size_t first = index;
    uint32_t value = 0;
    for (; index < size && text[index] >= '0' && text[index] <= '9'; index++) {
        value = value * 10U + (uint32_t)(text[index] - '0');
        if (value > backlight->max_brightness) return -BACKLIGHT_EINVAL;
    }
    if (index == first) return -BACKLIGHT_EINVAL;
    for (; index < size; index++)
        if (text[index] != '\n' && text[index] != ' ' && text[index] != '\0')
            return -BACKLIGHT_EINVAL;
    if (backlight->set(value) != 0) return -BACKLIGHT_EIO;
    backlight_level = value;
    return (int64_t)size;
}

static void backlight_attribute(struct vfs_node *parent, const char *name, uint64_t which,
                                int writable) {
    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE);
    if (!node) return;
    node->mode = writable ? 0644 : 0444;
    node->length = 64;
    node->read = backlight_read;
    if (writable) node->write = backlight_write;
    node->inode = which;
    if (vfs_attach(parent, node) != 0) vfs_free_node(node);
}

static struct pci_device backlight_parent;
static int backlight_has_parent;
static int sysfs_ready;

static void publish_backlight(void) {
    const struct backlight_device *device = backlight;
    const struct pci_device *parent = backlight_has_parent ? &backlight_parent : NULL;

    char devpath[128];
    size_t used = 0;
    if (parent) {
        append_string(devpath, sizeof(devpath), &used, "/devices/pci0000:00/");
        pci_slot_name(devpath, sizeof(devpath), &used, parent);
        append_string(devpath, sizeof(devpath), &used, "/backlight/");
    } else {
        append_string(devpath, sizeof(devpath), &used, "/devices/virtual/backlight/");
    }
    append_string(devpath, sizeof(devpath), &used, device->name);
    devpath[used] = '\0';

    char path[160];
    used = 0;
    append_string(path, sizeof(path), &used, "/sys");
    append_string(path, sizeof(path), &used, devpath);
    path[used] = '\0';
    struct vfs_node *directory = vfs_mkdir_p(path);
    if (!directory) return;

    backlight_level = device->get();
    backlight_attribute(directory, "brightness", BACKLIGHT_BRIGHTNESS, 1);
    backlight_attribute(directory, "actual_brightness", BACKLIGHT_ACTUAL, 0);
    backlight_attribute(directory, "max_brightness", BACKLIGHT_MAX, 0);
    backlight_attribute(directory, "type", BACKLIGHT_TYPE, 0);

    char file[176];
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/uevent");
    file[used] = '\0';
    static const char properties[] = "SUBSYSTEM=backlight\n";
    struct sysfs_device *registered =
        register_uevent(devpath, file, properties, sizeof(properties) - 1);

    (void)vfs_mkdir_p("/sys/class/backlight");
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/subsystem");
    file[used] = '\0';
    (void)vfs_create_symlink(file, "/sys/class/backlight", 0);

    char link[96];
    used = 0;
    append_string(link, sizeof(link), &used, "/sys/class/backlight/");
    append_string(link, sizeof(link), &used, device->name);
    link[used] = '\0';
    char target[160];
    used = 0;
    append_string(target, sizeof(target), &used, "../..");
    append_string(target, sizeof(target), &used, devpath);
    target[used] = '\0';
    (void)vfs_create_symlink(link, target, 0);

    if (registered) uevent_send(registered, "add");
}

#define BACKLIGHT_STEPS 10U

int backlight_step(int brighter) {
    VFS_GUARD;
    if (!backlight) return -1;
    uint32_t most = backlight->max_brightness;
    uint32_t step = most / BACKLIGHT_STEPS ? most / BACKLIGHT_STEPS : 1U;
    uint32_t level = backlight_level;
    if (brighter) level = most - level < step ? most : level + step;
    else level = level < step * 2U ? step : level - step;
    if (backlight->set(level) != 0) return -1;
    backlight_level = level;
    return 0;
}

void sysfs_publish_backlight(const struct backlight_device *device,
                             const struct pci_device *parent) {
    VFS_GUARD;
    if (backlight || !device || !device->get || !device->set) return;
    backlight = device;
    if (parent) {
        backlight_parent = *parent;
        backlight_has_parent = 1;
    }
    if (sysfs_ready) publish_backlight();
}

#define THERMAL_ZONES_MAX 16U

static thermal_zone_reader zone_reader;

static int64_t thermal_zone_read(struct vfs_node *node, uint64_t offset, size_t size,
                                 void *output) {
    char text[24];
    size_t length = 0;
    int32_t millicelsius = 0;
    if (!zone_reader || zone_reader((unsigned)node->inode, &millicelsius) != 0) return -5;
    if (millicelsius < 0) {
        append_string(text, sizeof(text), &length, "-");
        millicelsius = -millicelsius;
    }
    append_number(text, sizeof(text), &length, (uint32_t)millicelsius);
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

void sysfs_publish_thermal_zone(unsigned index, const char *type, thermal_zone_reader reader) {
    VFS_GUARD;
    if (index >= THERMAL_ZONES_MAX || !reader) return;
    zone_reader = reader;
    char name[32];
    size_t used = 0;
    append_string(name, sizeof(name), &used, "thermal_zone");
    append_number(name, sizeof(name), &used, index);
    name[used] = '\0';
    char path[96];
    used = 0;
    append_string(path, sizeof(path), &used, "/sys/devices/virtual/thermal/");
    append_string(path, sizeof(path), &used, name);
    path[used] = '\0';
    struct vfs_node *directory = vfs_mkdir_p(path);
    if (!directory) return;

    char file[128];
    char text[48];
    size_t length = 0;
    append_string(text, sizeof(text), &length, type);
    append_string(text, sizeof(text), &length, "\n");
    used = 0;
    append_string(file, sizeof(file), &used, path);
    append_string(file, sizeof(file), &used, "/type");
    file[used] = '\0';
    (void)vfs_create_file(file, text, length, 0, 1);

    struct vfs_node *temp = vfs_alloc_node("temp", VFS_FILE);
    if (temp) {
        temp->mode = 0444;
        temp->length = 64;
        temp->read = thermal_zone_read;
        temp->inode = index;
        if (vfs_attach(directory, temp) != 0) vfs_free_node(temp);
    }

    (void)vfs_mkdir_p("/sys/class/thermal");
    char link[96];
    used = 0;
    append_string(link, sizeof(link), &used, "/sys/class/thermal/");
    append_string(link, sizeof(link), &used, name);
    link[used] = '\0';
    char target[96];
    used = 0;
    append_string(target, sizeof(target), &used, "../../devices/virtual/thermal/");
    append_string(target, sizeof(target), &used, name);
    target[used] = '\0';
    (void)vfs_create_symlink(link, target, 0);
}

static struct vfs_node *tty0_active;

static int64_t tty0_active_read(struct vfs_node *node, uint64_t offset, size_t size, void *output) {
    (void)node;
    char text[16];
    size_t length = 0;
    append_string(text, sizeof(text), &length, "tty");
    append_number(text, sizeof(text), &length, vt_active_index());
    append_string(text, sizeof(text), &length, "\n");
    return attribute_reply(text, length, offset, size, output);
}

static void publish_console(void) {
    struct vfs_node *directory = vfs_mkdir_p("/sys/devices/virtual/tty/tty0");
    if (!directory) return;
    struct vfs_node *node = vfs_alloc_node("active", VFS_FILE);
    if (!node) return;
    node->mode = 0444;
    node->read = tty0_active_read;
    if (vfs_attach(directory, node) != 0) {
        vfs_free_node(node);
        return;
    }
    tty0_active = node;
    if (vfs_mkdir_p("/sys/class/tty"))
        (void)vfs_create_symlink("/sys/class/tty/tty0", "../../devices/virtual/tty/tty0", 0);
}

void sysfs_console_switched(void) { vfs_notify(tty0_active); }

void sysfs_init(void) {
    struct vfs_node *sys = vfs_mkdir_p("/sys");
    if (!sys) return;
    vfs_mount_builtin("sysfs", "/sys", "sysfs", sys);
    if (!vfs_mkdir_p("/sys/devices")) return;
    if (!vfs_mkdir_p("/sys/class")) return;
    if (!vfs_mkdir_p("/sys/dev/char")) return;
    if (!vfs_mkdir_p("/sys/dev/block")) return;
    if (!vfs_mkdir_p("/sys/module")) return;

    publish_pci_bus();
    publish_console();

    if (drm_available()) {
        int rendering = virtgpu_virgl_available();
        publish_device("card0", "dri/card0", "drm",
                       rendering ? "DRIVER=virtio_gpu\nDEVTYPE=drm_minor\n"
                                 : "DRIVER=tunixdrm\nDEVTYPE=drm_minor\n",
                       DEV_MAJOR_DRM, DEV_MINOR_DRM_CARD0);
        if (rendering) {
            publish_device("renderD128", "dri/renderD128", "drm",
                           "DRIVER=virtio_gpu\nDEVTYPE=drm_render_minor\n", DEV_MAJOR_DRM,
                           DEV_MINOR_DRM_RENDER0);
            publish_drm_nodes("card0", 2);
            publish_drm_nodes("renderD128", 2);
            publish_pci_parent("card0");
            publish_pci_parent("renderD128");
        } else {
            publish_drm_nodes("card0", 1);
            publish_platform_parent("card0");
        }
    }

    static const char *const input_names[] = {"Tunix keyboard", "Tunix mouse", "Tunix USB Tablet"};
    for (unsigned device = 0; device < 3U; device++) {
        char parent[48];
        size_t parent_used = 0;
        append_string(parent, sizeof(parent), &parent_used, "virtual/input/input");
        append_number(parent, sizeof(parent), &parent_used, device);
        parent[parent_used] = '\0';
        publish_input_parent(parent, input_names[device]);

        char name[64];
        size_t used = 0;
        append_string(name, sizeof(name), &used, parent);
        append_string(name, sizeof(name), &used, "/event");
        append_number(name, sizeof(name), &used, device);
        name[used] = '\0';
        char devname[40];
        size_t devname_used = 0;
        append_string(devname, sizeof(devname), &devname_used, "input/event");
        append_number(devname, sizeof(devname), &devname_used, device);
        devname[devname_used] = '\0';

        const char *tags =
            device == 0U ? "ID_INPUT=1\nID_INPUT_KEYBOARD=1\n" : "ID_INPUT=1\nID_INPUT_MOUSE=1\n";

        publish_device(name, devname, "input", tags, DEV_MAJOR_INPUT,
                       DEV_MINOR_INPUT_EVENT_BASE + device);
    }

    sysfs_ready = 1;
    if (backlight) publish_backlight();
}
