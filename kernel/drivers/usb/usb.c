#include <stddef.h>
#include <stdint.h>

#include "../../include/heap.h"
#include "../../include/kstring.h"
#include "../../include/usb.h"
#include "../../include/usb_storage.h"

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
    return slot_count++;
}

void usb_register_host(const struct usb_host *host) {
    if (!host || !host->storage_count || !host->bulk_transfer) return;
    if (add_host(host) != 0) return;
    int found = host->storage_count();
    for (int local = 0; local < found; local++) (void)add_slot(host, local);
}

void usb_host_storage_added(const struct usb_host *host, int local_index) {
    int registered = 0;
    for (int index = 0; index < host_count; index++)
        if (hosts[index] == host) registered = 1;
    if (!registered && add_host(host) != 0) return;
    int index = add_slot(host, local_index);
    if (index >= 0) usb_storage_attach(index);
}

int usb_storage_count(void) {
    return slot_count;
}

int usb_storage_present(int index) {
    if (index < 0 || index >= slot_count) return 0;
    const struct usb_host *host = slots[index].host;
    return host->present ? host->present(slots[index].local) : 1;
}

int usb_bulk_transfer(int index, int in, uint64_t physical, uint32_t length) {
    if (index < 0 || index >= slot_count) return -1;
    return slots[index].host->bulk_transfer(slots[index].local, in, physical, length);
}

int usb_reset_recovery(int index) {
    if (index < 0 || index >= slot_count) return -1;
    const struct usb_host *host = slots[index].host;
    if (!host->reset_recovery) return -1;
    return host->reset_recovery(slots[index].local);
}
