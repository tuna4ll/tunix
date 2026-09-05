#include <stddef.h>
#include <stdint.h>
#include "include/ahci.h"
#include "include/ata.h"
#include "include/block.h"
#include "include/kstring.h"
#include "include/nvme.h"
#include "include/partition.h"
#include "include/time.h"
#include "include/usb_storage.h"

/* See include/block.h for what this is and why the early boot read is not
   one of its clients. */

extern void kprintf(const char *fmt, ...);

static struct block_device devices[BLOCK_MAX_DEVICES];
static int device_count;
static int disk_count;
static int root_index;

/*
 * A partition is a device in its own right, and the only thing it owns is
 * where on its parent it starts. The parent index rather than a pointer: the
 * table is an array, and an entry added later must not be able to move one
 * that a partition is pointing at.
 */
struct partition_context {
    int parent;
    uint64_t start;
};

static struct partition_context partitions[BLOCK_MAX_DEVICES];

static int partition_read(void *context, uint64_t lba, uint32_t count,
                          void *destination) {
    const struct partition_context *part = context;
    const struct block_device *disk = block_device_at(part->parent);
    if (!disk) return -1;
    return disk->read(disk->context, part->start + lba, count, destination);
}

static int partition_write(void *context, uint64_t lba, uint32_t count,
                           const void *source) {
    const struct partition_context *part = context;
    const struct block_device *disk = block_device_at(part->parent);
    if (!disk || !disk->write) return -1;
    return disk->write(disk->context, part->start + lba, count, source);
}

static int partition_flush(void *context) {
    const struct partition_context *part = context;
    const struct block_device *disk = block_device_at(part->parent);
    if (!disk) return -1;
    return disk->flush ? disk->flush(disk->context) : 0;
}

static void announce(int index) {
    kprintf("BLOCK: %s (%s), %u sectors%s\n", devices[index].dev_name,
            devices[index].name, (unsigned)devices[index].sectors,
            devices[index].write ? "" : ", read only");
}

int block_register(const struct block_device *device) {
    if (!device || !device->read || !device->sectors) return -1;
    if (device_count == BLOCK_MAX_DEVICES) return -1;
    if (disk_count > 'z' - 'a') return -1;

    struct block_device *entry = &devices[device_count];
    *entry = *device;
    entry->name[sizeof entry->name - 1] = '\0';
    entry->parent = -1;
    entry->dev_name[0] = 's';
    entry->dev_name[1] = 'd';
    entry->dev_name[2] = (char)('a' + disk_count);
    entry->dev_name[3] = '\0';
    disk_count++;
    announce(device_count);
    return device_count++;
}

int block_register_partition(int parent, int number, uint64_t start,
                             uint64_t sectors) {
    const struct block_device *disk = block_device_at(parent);
    if (!disk || disk->parent != -1 || number < 1 || number > 9) return -1;
    if (!sectors || start >= disk->sectors || sectors > disk->sectors - start)
        return -1;
    if (device_count == BLOCK_MAX_DEVICES) return -1;

    struct partition_context *context = &partitions[device_count];
    context->parent = parent;
    context->start = start;

    struct block_device *entry = &devices[device_count];
    memset(entry, 0, sizeof *entry);
    memcpy(entry->name, disk->name, sizeof entry->name);
    memcpy(entry->dev_name, disk->dev_name, sizeof entry->dev_name);
    size_t length = strlen(entry->dev_name);
    entry->dev_name[length] = (char)('0' + number);
    entry->dev_name[length + 1] = '\0';
    entry->parent = parent;
    entry->sectors = sectors;
    entry->read = partition_read;
    entry->write = disk->write ? partition_write : NULL;
    entry->flush = partition_flush;
    entry->context = context;
    announce(device_count);
    return device_count++;
}

int block_device_index_by_name(const char *name) {
    if (!name) return -1;
    if (name[0] == '/') {
        static const char prefix[] = "/dev/";
        for (size_t index = 0; index < sizeof prefix - 1; index++)
            if (name[index] != prefix[index]) return -1;
        name += sizeof prefix - 1;
    }
    for (int index = 0; index < device_count; index++)
        if (strcmp(devices[index].dev_name, name) == 0) return index;
    return -1;
}

int block_device_count(void) { return device_count; }

const struct block_device *block_device_at(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

const struct block_device *block_root(void) {
    return root_index < device_count ? &devices[root_index] : NULL;
}

/* The root is device 0 unless something says otherwise. What used to decide
   it -- reading every disk for the boot manifest -- went away with the
   manifest; root= on the command line names the disk now, and a partition is
   a device of its own here, so the answer is an index. */
void block_select_root(int index) {
    root_index = index >= 0 && index < device_count ? index : 0;
    if (device_count) kprintf("BLOCK: root on %s\n", devices[root_index].dev_name);
}

/* What the medium was actually asked for. */
/* A read reaches the device with the kernel lock held and the driver waiting on
   it, so the count and the time are between them what a slow disk does to the
   whole machine rather than to one process. */
static uint64_t block_reads;
static uint64_t block_sectors_read;
static uint64_t block_read_ns;

void block_statistics(uint64_t *reads, uint64_t *sectors, uint64_t *nanoseconds) {
    if (reads) *reads = block_reads;
    if (sectors) *sectors = block_sectors_read;
    if (nanoseconds) *nanoseconds = block_read_ns;
}

static int device_read_counted(const struct block_device *device, uint64_t lba,
                               uint32_t count, void *destination) {
    uint64_t begun = time_uptime_ns();
    int status = device->read(device->context, lba, count, destination);
    uint64_t now = time_uptime_ns();
    block_reads++;
    block_sectors_read += count;
    if (now > begun) block_read_ns += now - begun;
    return status;
}

int block_read(uint64_t lba, uint32_t count, void *destination) {
    const struct block_device *device = block_root();
    if (!device || !count || !destination) return -1;
    if (lba + count > device->sectors) return -1;
    return device_read_counted(device, lba, count, destination);
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
        if (device_read_counted(device, lba, 1, sector) != 0) return -1;
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
            if (device_read_counted(device, lba, (uint32_t)whole, out) != 0) return -1;
            size_t moved = (size_t)whole * BLOCK_SECTOR_SIZE;
            out += moved;
            offset += moved;
            size -= moved;
            continue;
        }

        if (device_read_counted(device, lba, 1, sector) != 0) return -1;
        size_t chunk = BLOCK_SECTOR_SIZE - within;
        if (chunk > size) chunk = size;
        memcpy(out, sector + within, chunk);
        out += chunk;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

void block_probe(void) {
    ata_register_block_device();
    ahci_init();
    nvme_init();
    /* Before the partition scan and after the rest: the disks behind it hang
       off the xHCI controller main.c starts a few lines earlier. */
    usb_storage_init();
    partition_scan();
}
