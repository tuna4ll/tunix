#include <stddef.h>
#include <stdint.h>

#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/usb.h>
#include <tunix/usb_storage.h>
#include <tunix/sysfs.h>
#include <tunix/workqueue.h>
#include <tunix/lock.h>

#define USB_DEFAULT_TRANSFER 16384U

struct storage_slot {
    const struct usb_host *host;
    int local;
};

static const struct usb_host **hosts;
static int host_count;
static int host_capacity;
static struct storage_slot *slots;
static int slot_count;
static int slot_capacity;

static struct lock usb_lock = LOCK_INITIALIZER("usb hosts", LOCK_RANK_REGISTRY);

static void usb_guard_release(int *unused) {
    (void)unused;
    lock_release(&usb_lock);
}

#define USB_LOCKED \
    __attribute__((cleanup(usb_guard_release))) int usb_guard = (lock_acquire(&usb_lock), 0)

static int add_host(const struct usb_host *host) {
    if (host_count == host_capacity) {
        int capacity = host_capacity ? host_capacity * 2 : 4;
        const struct usb_host **grown = kmalloc((size_t)capacity * sizeof(*grown));
        if (!grown) return -1;
        if (host_count) memcpy(grown, hosts, (size_t)host_count * sizeof(*grown));
        kfree(hosts);
        hosts = grown;
        host_capacity = capacity;
    }
    hosts[host_count++] = host;
    return 0;
}

static int add_slot(const struct usb_host *host, int local) {
    if (slot_count == slot_capacity) {
        int capacity = slot_capacity ? slot_capacity * 2 : 8;
        struct storage_slot *grown = kmalloc((size_t)capacity * sizeof(*grown));
        if (!grown) return -1;
        if (slot_count) memcpy(grown, slots, (size_t)slot_count * sizeof(*grown));
        kfree(slots);
        slots = grown;
        slot_capacity = capacity;
    }
    slots[slot_count].host = host;
    slots[slot_count].local = local;
    int index = slot_count;
    __atomic_store_n(&slot_count, index + 1, __ATOMIC_RELEASE);
    return index;
}

void usb_register_host(const struct usb_host *host) {
    if (!host || !host->storage_count || !host->bulk_transfer) return;
    int found = host->storage_count();
    USB_LOCKED;
    if (add_host(host) != 0) return;
    for (int local = 0; local < found; local++) (void)add_slot(host, local);
}

void usb_host_storage_added(const struct usb_host *host, int local_index) {
    USB_LOCKED;
    int registered = 0;
    for (int index = 0; index < host_count; index++)
        if (hosts[index] == host) registered = 1;
    if (!registered && add_host(host) != 0) return;
    (void)add_slot(host, local_index);
}

int usb_storage_count(void) { return __atomic_load_n(&slot_count, __ATOMIC_ACQUIRE); }

static int slot_at(int index, struct storage_slot *out) {
    USB_LOCKED;
    if (index < 0 || index >= slot_count) return -1;
    *out = slots[index];
    return 0;
}

int usb_storage_present(int index) {
    struct storage_slot slot;
    if (slot_at(index, &slot) != 0) return 0;
    return slot.host->present ? slot.host->present(slot.local) : 1;
}

uint32_t usb_max_transfer(int index) {
    struct storage_slot slot;
    if (slot_at(index, &slot) != 0) return 0;
    return slot.host->max_transfer ? slot.host->max_transfer : USB_DEFAULT_TRANSFER;
}

int usb_bulk_transfer(int index, int in, uint64_t physical, uint32_t length) {
    struct storage_slot slot;
    if (slot_at(index, &slot) != 0) return -1;
    return slot.host->bulk_transfer(slot.local, in, physical, length);
}

int usb_reset_recovery(int index) {
    struct storage_slot slot;
    if (slot_at(index, &slot) != 0 || !slot.host->reset_recovery) return -1;
    return slot.host->reset_recovery(slot.local);
}

// ---- The device model ----

static struct usb_dev *usb_devices;
static struct usb_driver *usb_drivers;
static struct lock core_lock = LOCK_INITIALIZER("usb devices", LOCK_RANK_REGISTRY);

static int id_matches(const struct usb_device_id *id, const struct usb_interface_info *interface) {
    return (id->class_code == USB_ANY || id->class_code == interface->class_code) &&
        (id->subclass == USB_ANY || id->subclass == interface->subclass) &&
        (id->protocol == USB_ANY || id->protocol == interface->protocol);
}

static int driver_matches(const struct usb_driver *driver,
                          const struct usb_interface_info *interface) {
    for (unsigned index = 0; index < driver->id_count; index++)
        if (id_matches(&driver->ids[index], interface)) return 1;
    return 0;
}

// With the device's host locked: offers each unbound interface to DRIVER, or to every
// driver when DRIVER is NULL.
static void bind_device(struct usb_dev *device, struct usb_driver *only) {
    for (unsigned index = 0; index < device->interface_count; index++) {
        if (device->bound[index]) continue;
        lock_acquire(&core_lock);
        struct usb_driver *driver = only ? only : usb_drivers;
        for (; driver; driver = only ? NULL : driver->next)
            if (driver_matches(driver, &device->interfaces[index])) break;
        lock_release(&core_lock);
        if (!driver) continue;
        device->bound[index] = driver;
        if (driver->probe(device, index) != 0) {
            device->bound[index] = NULL;
            device->driver_data[index] = NULL;
        }
    }
}

// sysfs needs process context and the hosts call from under their locks, so the
// publishing happens later, in the workqueue.
#define USB_SYSFS_PENDING 64U

static struct {
    int add;
    char name[32];
    char modalias[96];
    char properties[192];
} pending[USB_SYSFS_PENDING];
static unsigned pending_head, pending_tail;

static void publish_pending(void *unused);
static struct work publish_work = WORK_INITIALIZER(publish_pending, NULL);

static void append_text(char *out, unsigned size, unsigned *used, const char *text) {
    while (*text && *used + 1U < size) out[(*used)++] = *text++;
    out[*used] = '\0';
}

static void append_hex(char *out, unsigned size, unsigned *used, unsigned value, unsigned digits) {
    static const char alphabet[] = "0123456789ABCDEF";
    while (digits--) {
        if (*used + 1U >= size) return;
        out[(*used)++] = alphabet[(value >> (digits * 4U)) & 0xFU];
    }
    out[*used] = '\0';
}

static void queue_sysfs(int add, const char *name, const char *modalias, const char *properties) {
    lock_acquire(&core_lock);
    if (pending_tail - pending_head < USB_SYSFS_PENDING) {
        unsigned slot = pending_tail++ % USB_SYSFS_PENDING;
        pending[slot].add = add;
        unsigned used = 0;
        append_text(pending[slot].name, sizeof(pending[slot].name), &used, name);
        used = 0;
        pending[slot].modalias[0] = '\0';
        append_text(pending[slot].modalias, sizeof(pending[slot].modalias), &used, modalias);
        used = 0;
        pending[slot].properties[0] = '\0';
        append_text(pending[slot].properties, sizeof(pending[slot].properties), &used, properties);
    }
    lock_release(&core_lock);
    work_queue(&publish_work);
}

static void publish_pending(void *unused) {
    (void)unused;
    for (;;) {
        char name[32], modalias[96], properties[192];
        int add;
        lock_acquire(&core_lock);
        if (pending_head == pending_tail) {
            lock_release(&core_lock);
            return;
        }
        unsigned slot = pending_head++ % USB_SYSFS_PENDING;
        add = pending[slot].add;
        memcpy(name, pending[slot].name, sizeof(name));
        memcpy(modalias, pending[slot].modalias, sizeof(modalias));
        memcpy(properties, pending[slot].properties, sizeof(properties));
        lock_release(&core_lock);
        if (add) sysfs_publish_usb(name, modalias, properties);
        else sysfs_remove_usb(name);
    }
}

// usb:vVVVVpPPPPdDDDDdcXXdscXXdpXXicXXiscXXipXXinXX, as Linux writes it.
static void interface_modalias(const struct usb_dev *device, const struct usb_interface_info *in,
                               char *out, unsigned size) {
    unsigned used = 0;
    out[0] = '\0';
    append_text(out, size, &used, "usb:v");
    append_hex(out, size, &used, device->vendor, 4);
    append_text(out, size, &used, "p");
    append_hex(out, size, &used, device->product, 4);
    append_text(out, size, &used, "d");
    append_hex(out, size, &used, device->release, 4);
    append_text(out, size, &used, "dc");
    append_hex(out, size, &used, device->class_code, 2);
    append_text(out, size, &used, "dsc");
    append_hex(out, size, &used, device->subclass, 2);
    append_text(out, size, &used, "dp");
    append_hex(out, size, &used, device->protocol, 2);
    append_text(out, size, &used, "ic");
    append_hex(out, size, &used, in->class_code, 2);
    append_text(out, size, &used, "isc");
    append_hex(out, size, &used, in->subclass, 2);
    append_text(out, size, &used, "ip");
    append_hex(out, size, &used, in->protocol, 2);
    append_text(out, size, &used, "in");
    append_hex(out, size, &used, in->number, 2);
}

static void interface_name(const struct usb_dev *device, unsigned index, char *out,
                           unsigned size) {
    unsigned used = 0;
    out[0] = '\0';
    append_text(out, size, &used, device->name);
    append_text(out, size, &used, ":1.");
    char digits[4];
    unsigned number = device->interfaces[index].number, count = 0;
    do {
        digits[count++] = (char)('0' + number % 10U);
        number /= 10U;
    } while (number && count < sizeof(digits));
    while (count && used + 1U < size) out[used++] = digits[--count];
    out[used] = '\0';
}

struct usb_dev *usb_device_added(const struct usb_host_ops *ops, void *host_device,
                                 const char *name, const uint8_t *descriptor,
                                 const struct usb_interface_info *interfaces, unsigned count) {
    if (!ops || !name || !descriptor) return NULL;
    struct usb_dev *device = kmalloc(sizeof(*device));
    if (!device) return NULL;
    memset(device, 0, sizeof(*device));
    device->ops = ops;
    device->host_device = host_device;
    unsigned used = 0;
    append_text(device->name, sizeof(device->name), &used, name);
    device->class_code = descriptor[4];
    device->subclass = descriptor[5];
    device->protocol = descriptor[6];
    device->vendor = (uint16_t)(descriptor[8] | (descriptor[9] << 8));
    device->product = (uint16_t)(descriptor[10] | (descriptor[11] << 8));
    device->release = (uint16_t)(descriptor[12] | (descriptor[13] << 8));
    if (count > USB_INTERFACES_MAX) count = USB_INTERFACES_MAX;
    for (unsigned index = 0; index < count; index++) device->interfaces[index] = interfaces[index];
    device->interface_count = count;

    lock_acquire(&core_lock);
    device->next = usb_devices;
    usb_devices = device;
    lock_release(&core_lock);

    char properties[192];
    used = 0;
    properties[0] = '\0';
    append_text(properties, sizeof(properties), &used, "DEVTYPE=usb_device\nPRODUCT=");
    append_hex(properties, sizeof(properties), &used, device->vendor, 4);
    append_text(properties, sizeof(properties), &used, "/");
    append_hex(properties, sizeof(properties), &used, device->product, 4);
    append_text(properties, sizeof(properties), &used, "\n");
    queue_sysfs(1, device->name, "", properties);
    for (unsigned index = 0; index < count; index++) {
        char interface[32], modalias[96];
        interface_name(device, index, interface, sizeof(interface));
        interface_modalias(device, &device->interfaces[index], modalias, sizeof(modalias));
        used = 0;
        properties[0] = '\0';
        append_text(properties, sizeof(properties), &used, "DEVTYPE=usb_interface\nINTERFACE=");
        append_hex(properties, sizeof(properties), &used, device->interfaces[index].class_code, 2);
        append_text(properties, sizeof(properties), &used, "/");
        append_hex(properties, sizeof(properties), &used, device->interfaces[index].subclass, 2);
        append_text(properties, sizeof(properties), &used, "/");
        append_hex(properties, sizeof(properties), &used, device->interfaces[index].protocol, 2);
        append_text(properties, sizeof(properties), &used, "\n");
        queue_sysfs(1, interface, modalias, properties);
    }
    bind_device(device, NULL);
    return device;
}

void usb_device_removed(struct usb_dev *device) {
    if (!device) return;
    for (unsigned index = 0; index < device->interface_count; index++) {
        if (device->bound[index] && device->bound[index]->disconnect)
            device->bound[index]->disconnect(device, index);
        device->bound[index] = NULL;
        char interface[32];
        interface_name(device, index, interface, sizeof(interface));
        queue_sysfs(0, interface, "", "");
    }
    queue_sysfs(0, device->name, "", "");
    lock_acquire(&core_lock);
    for (struct usb_dev **link = &usb_devices; *link; link = &(*link)->next) {
        if (*link != device) continue;
        *link = device->next;
        break;
    }
    lock_release(&core_lock);
    kfree(device);
}

static int device_listed(struct usb_dev *device) {
    lock_acquire(&core_lock);
    struct usb_dev *item = usb_devices;
    while (item && item != device) item = item->next;
    lock_release(&core_lock);
    return item != NULL;
}

// Runs FUNCTION on every device, each with its host locked; a device removed meanwhile
// is skipped. Devices are taken one at a time, newest first, and the walk restarts
// from the list after each, since the list can change while the core lock is not held.
static void for_each_device(void (*function)(struct usb_dev *device, struct usb_driver *driver),
                            struct usb_driver *driver) {
    struct usb_dev *seen[64];
    unsigned seen_count = 0;
    for (;;) {
        struct usb_dev *next = NULL;
        lock_acquire(&core_lock);
        for (struct usb_dev *item = usb_devices; item && !next; item = item->next) {
            unsigned index = 0;
            while (index < seen_count && seen[index] != item) index++;
            if (index == seen_count) next = item;
        }
        lock_release(&core_lock);
        if (!next || seen_count == 64) return;
        seen[seen_count++] = next;
        const struct usb_host_ops *ops = next->ops;
        ops->lock();
        if (device_listed(next)) function(next, driver);
        ops->unlock();
    }
}

static void bind_one(struct usb_dev *device, struct usb_driver *driver) { bind_device(device, driver); }

static void unbind_one(struct usb_dev *device, struct usb_driver *driver) {
    for (unsigned index = 0; index < device->interface_count; index++) {
        if (device->bound[index] != driver) continue;
        if (driver->disconnect) driver->disconnect(device, index);
        device->bound[index] = NULL;
        device->driver_data[index] = NULL;
    }
}

int usb_register_driver(struct usb_driver *driver) {
    if (!driver || !driver->probe) return -1;
    lock_acquire(&core_lock);
    driver->next = usb_drivers;
    usb_drivers = driver;
    lock_release(&core_lock);
    for_each_device(bind_one, driver);
    return 0;
}

void usb_unregister_driver(struct usb_driver *driver) {
    if (!driver) return;
    lock_acquire(&core_lock);
    for (struct usb_driver **link = &usb_drivers; *link; link = &(*link)->next) {
        if (*link != driver) continue;
        *link = driver->next;
        break;
    }
    lock_release(&core_lock);
    for_each_device(unbind_one, driver);
}
