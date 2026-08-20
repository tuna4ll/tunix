#ifndef TUNIX_AHCI_H
#define TUNIX_AHCI_H

/* Probe the AHCI controller and register every SATA disk behind it with the
   block layer. Safe to call on a machine that has none. */
void ahci_init(void);

#endif
