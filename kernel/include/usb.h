#ifndef TUNIX_USB_H
#define TUNIX_USB_H

#include <stdint.h>

/*
 * The seam between a USB host controller and the mass-storage transport above
 * it.
 *
 * It exists because there is now more than one controller. The transport used
 * to call xhci_bulk_transfer() by name, which was fine while xHCI was the only
 * way to reach a device and wrong the moment a machine old enough to have EHCI
 * instead needed to boot from its own USB stick.
 *
 * A host offers exactly two things: how many mass-storage devices it found,
 * and a synchronous bulk transfer to one of them. Enumeration, addressing and
 * endpoint setup stay inside the controller driver, because nothing above
 * cares and the two controllers do them in entirely different ways.
 */
struct usb_host {
    const char *name;
    int (*storage_count)(void);
    /* `physical` is a DMA address and `in` is 1 for device-to-host. Returns 0
       when the whole transfer completed. */
    int (*bulk_transfer)(int index, int in, uint64_t physical, uint32_t length);
    /* Put a device that failed midway through a command back where a new one
       can be sent to it. May be NULL on a controller that cannot. */
    int (*reset_recovery)(int index);
};

/* Called by a controller driver once it is running and has enumerated. */
void usb_register_host(const struct usb_host *host);

/* Every mass-storage device on every registered controller, numbered from
   zero in registration order. */
int usb_storage_count(void);
int usb_bulk_transfer(int index, int in, uint64_t physical, uint32_t length);
/* 0 when the device was reset, -1 when it could not be. */
int usb_reset_recovery(int index);

#endif
