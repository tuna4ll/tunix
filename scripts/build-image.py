#!/usr/bin/env python3
import binascii
import math
import struct
import sys

SECTOR_SIZE = 512
MANIFEST_LBA = 64
KERNEL_LBA = 65
KERNEL_SECTORS = 512
INITRAMFS_LBA = KERNEL_LBA + KERNEL_SECTORS
MANIFEST_MAGIC = 0x4D414E49
MANIFEST_VERSION = 3
# Must stay in sync with TUNIX_INITRAMFS_MAX_BYTES in
# src/kernel/include/boot_manifest.h.  The ceiling is set by stage2's low
# identity map (256 MiB) minus INITRAMFS_PHYSICAL (32 MiB), since the kernel
# loads the archive before installing its own page tables.
MAX_INITRAMFS_BYTES = 224 * 1024 * 1024
DATA_REGION_ALIGN_SECTORS = 2048
# The ext2 driver formats one block group per 128 MiB (a block bitmap is a
# single 4 KiB block, so 32768 blocks per group) and handles up to
# EXT2_MAX_GROUPS of them, which is 16 GiB. This value only decides how much of
# that the image actually reserves, and it pads the image file by the same
# amount. 512 MiB leaves real room above the ~115 MiB seeded tree.
DATA_REGION_BYTES = 512 * 1024 * 1024


def read(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def main() -> None:
    if len(sys.argv) != 6:
        raise SystemExit(f"usage: {sys.argv[0]} IMAGE STAGE1 STAGE2 KERNEL INITRAMFS")
    output, stage1_path, stage2_path, kernel_path, initramfs_path = sys.argv[1:]
    stage1 = read(stage1_path)
    stage2 = read(stage2_path)
    kernel = read(kernel_path)
    initramfs = read(initramfs_path)

    if len(stage1) != SECTOR_SIZE or stage1[510:512] != b"\x55\xaa":
        raise SystemExit("stage1 must be exactly 512 bytes with the 0xAA55 signature")
    if len(stage2) > 63 * SECTOR_SIZE:
        raise SystemExit("stage2 exceeds the 63-sector stage1 contract")
    if len(kernel) > KERNEL_SECTORS * SECTOR_SIZE:
        raise SystemExit(f"kernel.elf is {len(kernel)} bytes; stage2 limit is {KERNEL_SECTORS * SECTOR_SIZE}")

    stage2_sectors = math.ceil(len(stage2) / SECTOR_SIZE)
    initramfs_sectors = math.ceil(len(initramfs) / SECTOR_SIZE)
    if len(initramfs) > MAX_INITRAMFS_BYTES:
        raise SystemExit(
            f"initramfs is {len(initramfs)} bytes; early boot limit is "
            f"{MAX_INITRAMFS_BYTES} bytes ({MAX_INITRAMFS_BYTES // (1024 * 1024)} MiB)"
        )

    manifest_format = "<IHHQIQIQQIQII"
    manifest_size = struct.calcsize(manifest_format)
    manifest = struct.pack(
        manifest_format,
        MANIFEST_MAGIC,
        MANIFEST_VERSION,
        manifest_size,
        1,
        stage2_sectors,
        KERNEL_LBA,
        KERNEL_SECTORS,
        len(kernel),
        INITRAMFS_LBA,
        initramfs_sectors,
        len(initramfs),
        crc32(kernel),
        crc32(initramfs),
    ).ljust(SECTOR_SIZE, b"\0")

    minimum_size = (INITRAMFS_LBA + initramfs_sectors + 1) * SECTOR_SIZE
    data_lba = -(-(INITRAMFS_LBA + initramfs_sectors) // DATA_REGION_ALIGN_SECTORS) * DATA_REGION_ALIGN_SECTORS
    image_size = max(
        16 * 1024 * 1024,
        (minimum_size + 1024 * 1024 - 1) & ~(1024 * 1024 - 1),
        data_lba * SECTOR_SIZE + DATA_REGION_BYTES,
    )
    image = bytearray(image_size)
    image[0:SECTOR_SIZE] = stage1
    image[SECTOR_SIZE:SECTOR_SIZE + len(stage2)] = stage2
    image[MANIFEST_LBA * SECTOR_SIZE:(MANIFEST_LBA + 1) * SECTOR_SIZE] = manifest
    image[KERNEL_LBA * SECTOR_SIZE:KERNEL_LBA * SECTOR_SIZE + len(kernel)] = kernel
    image[INITRAMFS_LBA * SECTOR_SIZE:INITRAMFS_LBA * SECTOR_SIZE + len(initramfs)] = initramfs

    with open(output, "wb") as handle:
        handle.write(image)

    print(f"image: {output} ({image_size} bytes)")
    print(f"kernel: {len(kernel)}/{KERNEL_SECTORS * SECTOR_SIZE} bytes crc32={crc32(kernel):08x}")
    print(f"initramfs: {len(initramfs)} bytes/{initramfs_sectors} sectors crc32={crc32(initramfs):08x}")
    data_sectors = image_size // SECTOR_SIZE - data_lba
    print(f"ext2 data region: lba {data_lba}, {data_sectors} sectors ({data_sectors * SECTOR_SIZE // (1024 * 1024)} MiB)")


if __name__ == "__main__":
    main()
