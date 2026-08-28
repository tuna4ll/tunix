#include <stddef.h>
#include <stdint.h>

#include "../include/block.h"
#include "../include/kstring.h"

/*
 * Partition tables.
 *
 * The disk Limine boots from is partitioned: an EFI system partition holding
 * the bootloader and the kernel, and the root filesystem beside it. Without
 * this the kernel could only ever mount a filesystem that owned a whole disk,
 * which is not a layout any firmware will boot from.
 *
 * Both schemes are read because both are used: BIOS installs write an MBR,
 * UEFI wants GPT, and the image carries a protective MBR in front of the GPT
 * so that one disk boots either way.
 */

extern void kprintf(const char *fmt, ...);

#define MBR_SIGNATURE_OFFSET 0x1FEU
#define MBR_TABLE_OFFSET 0x1BEU
#define MBR_ENTRY_BYTES 16U
#define MBR_ENTRIES 4U
#define MBR_TYPE_PROTECTIVE 0xEEU
#define MBR_TYPE_EXTENDED_CHS 0x05U
#define MBR_TYPE_EXTENDED_LBA 0x0FU

#define GPT_HEADER_LBA 1U
#define GPT_SIGNATURE "EFI PART"
#define GPT_SIGNATURE_BYTES 8U

static uint32_t read_le32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
           ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static uint64_t read_le64(const uint8_t *at) {
    return (uint64_t)read_le32(at) | ((uint64_t)read_le32(at + 4) << 32);
}

static int signature_matches(const uint8_t *at, const char *signature, size_t bytes) {
    for (size_t index = 0; index < bytes; index++)
        if (at[index] != (uint8_t)signature[index]) return 0;
    return 1;
}

static int entry_is_empty(const uint8_t *guid) {
    for (unsigned index = 0; index < 16U; index++)
        if (guid[index]) return 0;
    return 1;
}

/*
 * Returns the number of partitions registered, or -1 when the disk has no GPT
 * -- which is not a failure, only an answer, and sends the caller to the MBR.
 */
static int scan_gpt(int disk, const struct block_device *device) {
    uint8_t header[BLOCK_SECTOR_SIZE];
    if (device->sectors <= GPT_HEADER_LBA) return -1;
    if (device->read(device->context, GPT_HEADER_LBA, 1, header) != 0) return -1;
    if (!signature_matches(header, GPT_SIGNATURE, GPT_SIGNATURE_BYTES)) return -1;

    uint64_t table_lba = read_le64(header + 72);
    uint32_t entries = read_le32(header + 80);
    uint32_t entry_bytes = read_le32(header + 84);
    if (!entry_bytes || entry_bytes > BLOCK_SECTOR_SIZE ||
        BLOCK_SECTOR_SIZE % entry_bytes != 0) return -1;
    /* The usual table is 128 entries and there is no reason to read a longer
       one: only the first nine can be named /dev/sdaN anyway. */
    if (entries > 128U) entries = 128U;

    uint32_t per_sector = BLOCK_SECTOR_SIZE / entry_bytes;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    int number = 0;
    int registered = 0;

    for (uint32_t index = 0; index < entries; index++) {
        if (index % per_sector == 0) {
            uint64_t lba = table_lba + index / per_sector;
            if (lba >= device->sectors) break;
            if (device->read(device->context, lba, 1, sector) != 0) break;
        }
        const uint8_t *entry = sector + (index % per_sector) * entry_bytes;
        number++;
        if (entry_is_empty(entry)) continue;

        uint64_t first = read_le64(entry + 32);
        uint64_t last = read_le64(entry + 40);
        if (last < first) continue;
        if (block_register_partition(disk, number, first, last - first + 1U) >= 0)
            registered++;
    }
    return registered;
}

static void scan_mbr(int disk, const struct block_device *device) {
    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (device->read(device->context, 0, 1, sector) != 0) return;
    if (sector[MBR_SIGNATURE_OFFSET] != 0x55U ||
        sector[MBR_SIGNATURE_OFFSET + 1U] != 0xAAU) return;

    for (unsigned index = 0; index < MBR_ENTRIES; index++) {
        const uint8_t *entry = sector + MBR_TABLE_OFFSET + index * MBR_ENTRY_BYTES;
        uint8_t type = entry[4];
        if (!type || type == MBR_TYPE_PROTECTIVE) continue;
        /* Extended partitions are a linked list inside one primary entry.
           Nothing this kernel boots from uses them, and following the chain
           for its own sake would be code with no caller. */
        if (type == MBR_TYPE_EXTENDED_CHS || type == MBR_TYPE_EXTENDED_LBA) continue;

        uint64_t start = read_le32(entry + 8);
        uint64_t sectors = read_le32(entry + 12);
        if (!sectors) continue;
        (void)block_register_partition(disk, (int)index + 1, start, sectors);
    }
}

/*
 * Every disk registered so far. Partitions are appended to the same table, and
 * the loop stops at the count taken before it started, so a partition is never
 * itself scanned for partitions.
 */
void partition_scan(void) {
    int disks = block_device_count();
    for (int index = 0; index < disks; index++) {
        const struct block_device *device = block_device_at(index);
        if (!device || device->parent != -1) continue;
        if (scan_gpt(index, device) >= 0) continue;
        scan_mbr(index, device);
    }
}
