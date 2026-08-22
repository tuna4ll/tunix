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
 * The early boot path is deliberately *not* a client. kmain reads the manifest
 * and the initramfs before the page tables and the allocator exist, which a
 * memory-mapped controller cannot serve, so that one read stays on port-I/O
 * IDE. See main.c.
 */

#define BLOCK_SECTOR_SIZE 512U
#define BLOCK_MAX_DEVICES 8

struct block_device {
    /* Stable enough to print: "ide0", "ahci0", "nvme0", "usb0". */
    char name[16];
    uint64_t sectors;
    /* Non-zero on failure. `count` is in 512-byte sectors and is never 0. */
    int (*read)(void *context, uint64_t lba, uint32_t count, void *destination);
    int (*write)(void *context, uint64_t lba, uint32_t count, const void *source);
    /* Push whatever the controller is holding to the medium. May be NULL. */
    int (*flush)(void *context);
    void *context;
};

/* Returns the index the device was given, or -1 when the table is full. */
int block_register(const struct block_device *device);
int block_device_count(void);
const struct block_device *block_device_at(int index);

/*
 * Choose the device the root filesystem lives on. Every registered disk is
 * asked whether it carries our boot manifest, and the first that says yes wins;
 * that is a fact about the medium rather than a guess from a preference order,
 * which matters as soon as a machine has a scratch NVMe drive alongside the
 * disk it actually booted from. Falls back to device 0.
 */
void block_select_root(uint32_t manifest_lba);
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

/* Every controller that can answer before the allocator and the page tables
   exist. Called from kmain before the manifest is read. */
void block_probe_early(void);
/* Called once the kernel's own page tables are up: moves register windows out
   of the identity map, and brings up the controllers that could not be probed
   earlier. */
void block_probe_controllers(void);

/* The sector the bootloader read the manifest from; defined in tunix_boot.c. */
uint32_t tunix_boot_manifest_lba(void);

#endif
