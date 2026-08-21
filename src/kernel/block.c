#include <stddef.h>
#include <stdint.h>
#include "include/ahci.h"
#include "include/block.h"
#include "include/boot_manifest.h"
#include "include/kstring.h"
#include "include/nvme.h"
#include "include/usb_storage.h"

/* See include/block.h for what this is and why the early boot read is not
   one of its clients. */

extern void kprintf(const char *fmt, ...);

static struct block_device devices[BLOCK_MAX_DEVICES];
static int device_count;
static int root_index;

int block_register(const struct block_device *device) {
    if (!device || !device->read || !device->sectors) return -1;
    if (device_count == BLOCK_MAX_DEVICES) return -1;
    devices[device_count] = *device;
    devices[device_count].name[sizeof(devices[0].name) - 1] = '\0';
    kprintf("BLOCK: %s, %u sectors%s\n", devices[device_count].name,
            (unsigned)devices[device_count].sectors,
            devices[device_count].write ? "" : ", read only");
    return device_count++;
}

int block_device_count(void) { return device_count; }

const struct block_device *block_device_at(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

const struct block_device *block_root(void) {
    return root_index < device_count ? &devices[root_index] : NULL;
}

/*
 * The manifest the bootloader wrote is the only thing that identifies our disk
 * among several, so the root is found by reading for it rather than by ranking
 * controller types -- which matters the moment a machine has a scratch drive
 * next to the one it booted from.
 */
static int carries_manifest(const struct block_device *device, uint32_t lba) {
    if (device->sectors <= lba) return 0;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (device->read(device->context, lba, 1, sector) != 0) return 0;
    uint32_t magic;
    memcpy(&magic, sector, sizeof(magic));
    return magic == TUNIX_MANIFEST_MAGIC;
}

void block_select_root(uint32_t manifest_lba) {
    for (int index = 0; index < device_count; index++) {
        if (!carries_manifest(&devices[index], manifest_lba)) continue;
        root_index = index;
        kprintf("BLOCK: root on %s\n", devices[index].name);
        return;
    }
    root_index = 0;
    if (device_count)
        kprintf("BLOCK: no manifest found, root on %s\n", devices[0].name);
}

int block_read(uint64_t lba, uint32_t count, void *destination) {
    const struct block_device *device = block_root();
    if (!device || !count || !destination) return -1;
    if (lba + count > device->sectors) return -1;
    return device->read(device->context, lba, count, destination);
}

int block_write(uint64_t lba, uint32_t count, const void *source) {
    const struct block_device *device = block_root();
    if (!device || !device->write || !count || !source) return -1;
    if (lba + count > device->sectors) return -1;
    return device->write(device->context, lba, count, source);
}

int block_flush(void) {
    const struct block_device *device = block_root();
    if (!device) return -1;
    return device->flush ? device->flush(device->context) : 0;
}

uint64_t block_sectors(void) {
    const struct block_device *device = block_root();
    return device ? device->sectors : 0;
}

/*
 * Byte-granular reads for /dev/sda, which is a character-like view of a device
 * that only moves whole sectors. The staging buffer is one sector, so a read
 * that starts or ends mid-sector costs one extra transfer rather than a
 * bounce of the whole request.
 */
int block_read_bytes(uint64_t offset, size_t size, void *destination) {
    return block_device_read_bytes(block_root(), offset, size, destination);
}

int block_device_write_bytes(const struct block_device *device, uint64_t offset,
                             size_t size, const void *source) {
    if (!device || !device->write || !source) return -1;
    const uint8_t *in = (const uint8_t *)source;
    uint8_t sector[BLOCK_SECTOR_SIZE];

    while (size) {
        uint64_t lba = offset / BLOCK_SECTOR_SIZE;
        size_t within = (size_t)(offset % BLOCK_SECTOR_SIZE);
        if (lba >= device->sectors) return -1;

        if (!within && size >= BLOCK_SECTOR_SIZE) {
            uint64_t whole = size / BLOCK_SECTOR_SIZE;
            if (whole > device->sectors - lba) whole = device->sectors - lba;
            if (whole > 0xFFFFU) whole = 0xFFFFU;
            if (device->write(device->context, lba, (uint32_t)whole, in) != 0) return -1;
            size_t moved = (size_t)whole * BLOCK_SECTOR_SIZE;
            in += moved;
            offset += moved;
            size -= moved;
            continue;
        }

        /* A partial sector is somebody else's data either side of it. */
        if (device->read(device->context, lba, 1, sector) != 0) return -1;
        size_t chunk = BLOCK_SECTOR_SIZE - within;
        if (chunk > size) chunk = size;
        memcpy(sector + within, in, chunk);
        if (device->write(device->context, lba, 1, sector) != 0) return -1;
        in += chunk;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

int block_device_read_bytes(const struct block_device *device, uint64_t offset,
                            size_t size, void *destination) {
    if (!device || !destination) return -1;
    uint8_t *out = (uint8_t *)destination;
    uint8_t sector[BLOCK_SECTOR_SIZE];

    while (size) {
        uint64_t lba = offset / BLOCK_SECTOR_SIZE;
        size_t within = (size_t)(offset % BLOCK_SECTOR_SIZE);
        if (lba >= device->sectors) return -1;

        if (!within && size >= BLOCK_SECTOR_SIZE) {
            /* A whole run of sectors, straight into the caller's buffer. */
            uint64_t whole = size / BLOCK_SECTOR_SIZE;
            if (whole > device->sectors - lba) whole = device->sectors - lba;
            if (whole > 0xFFFFU) whole = 0xFFFFU;
            if (device->read(device->context, lba, (uint32_t)whole, out) != 0) return -1;
            size_t moved = (size_t)whole * BLOCK_SECTOR_SIZE;
            out += moved;
            offset += moved;
            size -= moved;
            continue;
        }

        if (device->read(device->context, lba, 1, sector) != 0) return -1;
        size_t chunk = BLOCK_SECTOR_SIZE - within;
        if (chunk > size) chunk = size;
        memcpy(out, sector + within, chunk);
        out += chunk;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

void block_probe_controllers(void) {
    /* IDE is already here: main.c registered it before the allocator existed,
       because the initramfs read needed it. These could not have been probed
       then -- AHCI and NVMe are memory mapped, and the USB one needs the
       controller the line before it in main.c brought up. */
    ahci_init();
    nvme_init();
    usb_storage_init();
}
