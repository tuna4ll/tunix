#ifndef TUNIX_USB_H
#define TUNIX_USB_H

#include <stdint.h>

struct usb_host {
    const char *name;
    int (*storage_count)(void);
    int (*bulk_transfer)(int index, int in, uint64_t physical, uint32_t length);
    int (*reset_recovery)(int index);
    int (*present)(int index);
};

void usb_register_host(const struct usb_host *host);
void usb_host_storage_added(const struct usb_host *host, int local_index);

int usb_storage_count(void);
int usb_bulk_transfer(int index, int in, uint64_t physical, uint32_t length);
int usb_reset_recovery(int index);
int usb_storage_present(int index);

#endif
