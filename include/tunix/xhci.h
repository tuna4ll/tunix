#ifndef TUNIX_XHCI_H
#define TUNIX_XHCI_H

int xhci_init(void);
void xhci_poll(void);
void xhci_rescan(void);

#endif
