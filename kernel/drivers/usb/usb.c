#include <stddef.h>
#include <stdint.h>

#include "../../include/heap.h"
#include "../../include/kstring.h"
#include "../../include/usb.h"
#include "../../include/usb_storage.h"
#include "../../include/lock.h"

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

int usb_storage_count(void) {
    return __atomic_load_n(&slot_count, __ATOMIC_ACQUIRE);
}

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
