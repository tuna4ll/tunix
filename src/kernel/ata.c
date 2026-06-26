#include <stddef.h>
#include <stdint.h>
#include "include/ata.h"
#include "include/io.h"
#include "include/kstring.h"

#define ATA_DATA       0x1F0
#define ATA_SECCOUNT0  0x1F2
#define ATA_LBA0       0x1F3
#define ATA_LBA1       0x1F4
#define ATA_LBA2       0x1F5
#define ATA_HDDEVSEL   0x1F6
#define ATA_COMMAND    0x1F7
#define ATA_STATUS     0x1F7

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_SR_BSY       0x80
#define ATA_SR_DRQ       0x08
#define ATA_SR_DF        0x20
#define ATA_SR_ERR       0x01
#define ATA_SECTOR_SIZE  512U

static uint32_t cached_sectors;
static int identify_attempted;

static int ata_wait_not_busy(void) {
    for (uint32_t timeout = 0; timeout < 10000000U; timeout++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_SR_BSY)) return status;
        __asm__ volatile("pause");
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t timeout = 0; timeout < 10000000U; timeout++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

uint32_t ata_disk_sectors(void) {
    if (identify_attempted) return cached_sectors;
    identify_attempted = 1;

    outb(ATA_HDDEVSEL, 0xA0U);
    io_wait();
    outb(ATA_SECCOUNT0, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) return 0;
    if (ata_wait_not_busy() < 0) return 0;
    if (inb(ATA_LBA1) != 0 || inb(ATA_LBA2) != 0) return 0;
    if (ata_wait_drq() != 0) return 0;

    uint16_t identify[256];
    for (unsigned i = 0; i < 256U; i++) identify[i] = inw(ATA_DATA);
    cached_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    if (cached_sectors > 0x10000000U) cached_sectors = 0x10000000U;
    return cached_sectors;
}

int ata_pio_read28(uint32_t lba, uint32_t sectors, void *destination) {
    if (!destination || sectors == 0 || lba > 0x0FFFFFFFU ||
        sectors > 0x10000000U - lba) return -1;

    uint32_t disk_sectors = ata_disk_sectors();
    if (disk_sectors && (lba >= disk_sectors || sectors > disk_sectors - lba)) return -1;

    uint16_t *output = (uint16_t *)destination;
    uint32_t remaining = sectors;
    uint32_t current_lba = lba;

    while (remaining) {
        uint8_t batch = (uint8_t)(remaining > 255U ? 255U : remaining);

        outb(ATA_HDDEVSEL, (uint8_t)(0xE0U | ((current_lba >> 24) & 0x0FU)));
        io_wait();
        outb(ATA_SECCOUNT0, batch);
        outb(ATA_LBA0, (uint8_t)current_lba);
        outb(ATA_LBA1, (uint8_t)(current_lba >> 8));
        outb(ATA_LBA2, (uint8_t)(current_lba >> 16));
        outb(ATA_COMMAND, ATA_CMD_READ_PIO);

        for (uint32_t sector = 0; sector < batch; sector++) {
            if (ata_wait_drq() != 0) return -1;
            for (uint32_t word = 0; word < 256U; word++) *output++ = inw(ATA_DATA);
            io_wait();
        }

        current_lba += batch;
        remaining -= batch;
    }
    return 0;
}

int ata_pio_read_bytes(uint64_t offset, size_t size, void *destination) {
    if (!destination) return -1;
    if (!size) return 0;

    uint32_t sectors = ata_disk_sectors();
    uint64_t disk_size = (uint64_t)sectors * ATA_SECTOR_SIZE;
    if (!sectors || offset >= disk_size) return 0;
    if ((uint64_t)size > disk_size - offset) size = (size_t)(disk_size - offset);

    uint8_t bounce[ATA_SECTOR_SIZE];
    uint8_t *out = (uint8_t *)destination;
    size_t completed = 0;
    while (completed < size) {
        uint64_t absolute = offset + completed;
        uint32_t lba = (uint32_t)(absolute / ATA_SECTOR_SIZE);
        size_t in_sector = (size_t)(absolute % ATA_SECTOR_SIZE);
        size_t chunk = ATA_SECTOR_SIZE - in_sector;
        if (chunk > size - completed) chunk = size - completed;
        if (ata_pio_read28(lba, 1, bounce) != 0) return completed ? (int)completed : -1;
        memcpy(out + completed, bounce + in_sector, chunk);
        completed += chunk;
    }
    return (int)completed;
}
