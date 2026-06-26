#ifndef TUNIX_BOOT_MANIFEST_H
#define TUNIX_BOOT_MANIFEST_H

#include <stdint.h>

#define TUNIX_MANIFEST_MAGIC 0x4D414E49U
#define TUNIX_MANIFEST_VERSION 3U
#define TUNIX_INITRAMFS_MAX_BYTES (16ULL * 1024ULL * 1024ULL)
#define TUNIX_INITRAMFS_MAX_SECTORS (TUNIX_INITRAMFS_MAX_BYTES / 512ULL)

struct boot_manifest {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint64_t stage2_lba;
    uint32_t stage2_sectors;
    uint64_t kernel_lba;
    uint32_t kernel_sectors;
    uint64_t kernel_size;
    uint64_t initramfs_lba;
    uint32_t initramfs_sectors;
    uint64_t initramfs_size;
    uint32_t kernel_crc32;
    uint32_t initramfs_crc32;
} __attribute__((packed));

#endif
