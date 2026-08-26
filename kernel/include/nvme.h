#ifndef TUNIX_NVME_H
#define TUNIX_NVME_H

/*
 * Probe the NVMe controller and register its first namespace with the block
 * layer. Safe to call on a machine that has none.
 *
 * Runs before the allocator and the kernel's page tables, for the reason given
 * above ahci_init(): a machine whose only disk is NVMe has nothing else to read
 * its manifest with.
 */
void nvme_init(void);

/* Move the register window into the device area once the kernel's own page
   tables are up. */

#endif
