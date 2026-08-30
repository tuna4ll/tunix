/*
 * The list of USB host controllers, and the numbering above them.
 *
 * A device index handed down from the mass-storage transport is a position in
 * one flat list: controllers are asked in the order they registered, and each
 * one's devices occupy as many consecutive numbers as it found. That keeps the
 * transport free of any notion of which controller a disk is on, which is the
 * whole point of the seam.
 */
#include <stddef.h>
#include <stdint.h>

#include "../../include/usb.h"

#define USB_MAX_HOSTS 4

static const struct usb_host *hosts[USB_MAX_HOSTS];
static int host_count;

void usb_register_host(const struct usb_host *host) {
    if (!host || !host->storage_count || !host->bulk_transfer) return;
    if (host_count >= USB_MAX_HOSTS) return;
    hosts[host_count++] = host;
}

int usb_storage_count(void) {
    int total = 0;
    for (int index = 0; index < host_count; index++)
        total += hosts[index]->storage_count();
    return total;
}

int usb_bulk_transfer(int index, int in, uint64_t physical, uint32_t length) {
    if (index < 0) return -1;
    for (int host = 0; host < host_count; host++) {
        int found = hosts[host]->storage_count();
        if (index < found)
            return hosts[host]->bulk_transfer(index, in, physical, length);
        index -= found;
    }
    return -1;
}

int usb_reset_recovery(int index) {
    if (index < 0) return -1;
    for (int host = 0; host < host_count; host++) {
        int found = hosts[host]->storage_count();
        if (index < found) {
            if (!hosts[host]->reset_recovery) return -1;
            return hosts[host]->reset_recovery(index);
        }
        index -= found;
    }
    return -1;
}
