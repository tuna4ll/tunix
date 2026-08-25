#ifndef TUNIX_BLOCK_H
#define TUNIX_BLOCK_H

#include <stddef.h>
#include <stdint.h>

/*
 * The block layer: one interface over every disk the kernel can reach.
 *
 * It exists because the filesystem used to call the IDE driver by name, so a
 * machine whose only disk is SATA or NVMe -- which is most of them, and every
 * `-machine q35` -- had no storage at all. Drivers register here; ext2 and
 * /dev/sda go through here and never learn which kind of controller answered.
 *
 * Everything is counted in 512-byte sectors, including for an NVMe namespace
 * formatted with 4 KiB blocks: the driver does the translation, because the
 * alternative is every caller knowing the geometry of every controller.
 *
 * There is no longer an early path around it. The kernel used to read a boot
 * manifest and an initramfs off the disk before the allocator existed, which
 * only port-I/O IDE could serve; Limine loads the kernel and the root lives on
 * a real partition, so the first disk read now happens with everything up.
 */

#define BLOCK_SECTOR_SIZE 512U
/* Eight disks was the old limit, from before a partition was a device. A disk
   with four partitions is five entries now. */
#define BLOCK_MAX_DEVICES 32
#define BLOCK_NAME_BYTES 8

struct block_device {
    /* What the driver calls it: "ide0", "ahci0", "nvme0", "usb0". */
    char name[16];
    /* What /dev calls it -- "sda", "sda1" -- assigned at registration so that
       the block layer and devfs cannot disagree about which disk is which. */
    char dev_name[BLOCK_NAME_BYTES];
    /* -1 for a whole disk, otherwise the index of the disk this is a partition
       of. Partitions read and write through their parent with an offset. */
    int parent;
    uint64_t sectors;
    /* Non-zero on failure. `count` is in 512-byte sectors and is never 0. */
    int (*read)(void *context, uint64_t lba, uint32_t count, void *destination);
    int (*write)(void *context, uint64_t lba, uint32_t count, const void *source);
    /* Push whatever the controller is holding to the medium. May be NULL. */
    int (*flush)(void *context);
    void *context;
};

/* Returns the index the disk was given, or -1 when the table is full. */
int block_register(const struct block_device *device);
/* One partition of an already registered disk, counted from 1. Reads and
   writes are the parent's, shifted by `start`. */
int block_register_partition(int parent, int number, uint64_t start,
                             uint64_t sectors);
int block_device_count(void);
const struct block_device *block_device_at(int index);
/* Look a device up by what /dev calls it: "sda", "sda2". -1 if there is no
   such device. A leading "/dev/" is accepted and ignored. */
int block_device_index_by_name(const char *name);

/* Choose the device the root filesystem lives on, by index. Falls back to
   device 0 when the index names no disk. */
void block_select_root(int index);
const struct block_device *block_root(void);

/* The root device, for the filesystem and /dev/sda. */
int block_read(uint64_t lba, uint32_t count, void *destination);
int block_write(uint64_t lba, uint32_t count, const void *source);
int block_read_bytes(uint64_t offset, size_t size, void *destination);
/* Byte-granular access to any registered disk, for /dev/sd* and for a
   filesystem whose structures are not sector aligned. A write that does not
   cover whole sectors reads them back first. */
int block_device_read_bytes(const struct block_device *device, uint64_t offset,
                            size_t size, void *destination);
int block_device_write_bytes(const struct block_device *device, uint64_t offset,
                             size_t size, const void *source);
int block_flush(void);
uint64_t block_sectors(void);

/* Bring up every controller, register the disks behind it, and read each
   disk's partition table. Called once the allocator and the kernel's own page
   tables exist, which all three of the memory-mapped controllers need. */
void block_probe(void);

#endif
