#ifndef TUNIX_USB_STORAGE_H
#define TUNIX_USB_STORAGE_H

/* Register every USB mass-storage device the xHCI driver enumerated with the
   block layer. Safe to call when there are none. */
void usb_storage_init(void);

#endif
