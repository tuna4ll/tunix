#ifndef TUNIX_TARFS_H
#define TUNIX_TARFS_H

#include <stdint.h>

int tarfs_unpack(uint64_t initramfs_physical_address, uint64_t initramfs_size);

#endif
