#!/usr/bin/env python3
import binascii
import math
import struct
import sys

SECTOR_SIZE = 512
MANIFEST_LBA = 64
KERNEL_LBA = 65
KERNEL_SECTORS = 192
INITRAMFS_LBA = KERNEL_LBA + KERNEL_SECTORS
MANIFEST_MAGIC = 0x4D414E49
MANIFEST_VERSION = 3
MAX_INITRAMFS_BYTES = 16 * 1024 * 1024


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
            f"{MAX_INITRAMFS_BYTES} bytes (16 MiB)"
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
    image_size = max(16 * 1024 * 1024, (minimum_size + 1024 * 1024 - 1) & ~(1024 * 1024 - 1))
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


if __name__ == "__main__":
    main()
