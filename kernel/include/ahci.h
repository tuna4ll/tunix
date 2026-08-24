#ifndef TUNIX_AHCI_H
#define TUNIX_AHCI_H

/*
 * Probe the AHCI controller and register every SATA disk behind it with the
 * block layer. Safe to call on a machine that has none.
 *
 * This runs *before* the allocator and the kernel's own page tables, because
 * the manifest and the initramfs are read before both and a machine whose only
 * disk is SATA has nothing else to read them with. Two things make that
 * possible: the loader leaves a four-gigabyte identity map in place, so the
 * register window is reachable at its physical address; and the DMA it needs is
 * a static buffer in the kernel image rather than pages from an allocator that
 * does not exist yet.
 */
void ahci_init(void);

/* Move the register window into the device area once the kernel's own page
   tables are up. The identity map the probe used belongs to the low half of
   the address space, which becomes user memory the moment a process runs. */
void ahci_remap(void);

#endif
