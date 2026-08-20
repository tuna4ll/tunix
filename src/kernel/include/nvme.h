#ifndef TUNIX_NVME_H
#define TUNIX_NVME_H

/* Probe the NVMe controller and register its first namespace with the block
   layer. Safe to call on a machine that has none. */
void nvme_init(void);

#endif
