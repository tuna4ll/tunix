#ifndef TUNIX_EHCI_H
#define TUNIX_EHCI_H

/*
 * Bring up every EHCI controller on the machine and enumerate the mass storage
 * behind it. Everything above reaches those disks through usb.h; there is
 * nothing else here to ask for. See drivers/usb/ehci.c.
 */
int ehci_init(void);

#endif
