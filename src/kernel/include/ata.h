#ifndef TUNIX_ATA_H
#define TUNIX_ATA_H

#include <stddef.h>
#include <stdint.h>

int ata_pio_read28(uint32_t lba, uint32_t sectors, void *destination);
int ata_pio_read_bytes(uint64_t offset, size_t size, void *destination);
uint32_t ata_disk_sectors(void);

#endif
