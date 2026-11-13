#ifndef TUNIX_XHCI_H
#define TUNIX_XHCI_H

int xhci_init(void);
void xhci_poll(void);
void xhci_rescan(void);
int xhci_keyboard_present(void);
int xhci_pointer_present(void);

#endif
