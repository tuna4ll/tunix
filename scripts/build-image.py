#!/usr/bin/env python3
"""Lay out a disk tunix-boot can boot.

The loader reads a filesystem, so the kernel goes in a FAT32 partition. The
initramfs does not: it is hundreds of megabytes and the kernel loads it itself
with its own ATA driver, straight to a fixed physical address, long before it
has a filesystem of any kind. So it stays where it always was — at a raw sector,
with a manifest sector in front of it saying where.

    LBA 0                  tunix-boot boot record, carrying the partition table
    LBA 1                  tunix-boot stage2
    LBA 2048               FAT32: /tunix.cfg and /kernel.elf
    MANIFEST_LBA           the manifest sector
    MANIFEST_LBA + 1       the initramfs
    then                   the ext2 data region
"""

import binascii
import importlib.util
import math
import pathlib
import struct
import sys

SECTOR_SIZE = 512

PARTITION_LBA = 2048
PARTITION_SECTORS = 8192          # 4 MiB, room for a kernel many times over
PARTITION_TYPE_FAT32_LBA = 0x0C

TABLE_OFFSET = 446
TABLE_ENTRY_BYTES = 16
SIGNATURE_OFFSET = 510

MANIFEST_LBA = PARTITION_LBA + PARTITION_SECTORS
INITRAMFS_LBA = MANIFEST_LBA + 1

MANIFEST_MAGIC = 0x4D414E49
MANIFEST_VERSION = 3

# Must stay in sync with TUNIX_INITRAMFS_MAX_BYTES in
# src/kernel/include/boot_manifest.h.  The archive is loaded through the
# loader's identity map, which reaches 4 GiB, so the ceiling is about having
# the memory rather than about addressing it.
MAX_INITRAMFS_BYTES = 768 * 1024 * 1024
DATA_REGION_ALIGN_SECTORS = 2048
# The ext2 driver formats one block group per 128 MiB and handles up to
# EXT2_MAX_GROUPS (128) of them, so 16 GiB. This only decides how much the
# image reserves -- and it has to hold the whole initramfs, because first boot
# seeds the root filesystem from it and then everything the user writes.
#
# 1 GiB left almost nothing over once the seeded root was in place: installing
# gcc alone spends a couple of hundred megabytes, and a desktop's worth of
# packages does not fit at all. The region is written as a hole rather than as
# zeroes (see below), so the reservation costs image file length and not disk.
DATA_REGION_BYTES = 8 * 1024 * 1024 * 1024

CONFIG_NAME = b"TUNIX   CFG"
KERNEL_NAME = b"KERNEL  ELF"

# The manifest is not in the filesystem, so the loader cannot name it. The
# kernel is told where it is the one way the loader carries an arbitrary
# string: the command line.
CONFIG_TEMPLATE = """# what to boot
timeout = 0
default = tunix

:tunix
kernel = /kernel.elf
cmdline = manifest_lba={manifest_lba}
"""


def load_fat_builder(root: pathlib.Path):
    """tunix-boot's own image tool, imported for its FAT32 writer.

    Reused rather than reimplemented: two filesystem writers that have to agree
    about a layout are two chances to disagree about one.
    """
    path = root / "tunix-boot" / "tools" / "make-image.py"
    if not path.exists():
        raise SystemExit(f"{path} is missing; is the submodule checked out?")

    spec = importlib.util.spec_from_file_location("tunix_boot_image", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def write_partition_entry(boot_record: bytearray) -> None:
    entry = bytearray(TABLE_ENTRY_BYTES)
    entry[4] = PARTITION_TYPE_FAT32_LBA
    entry[8:12] = PARTITION_LBA.to_bytes(4, "little")
    entry[12:16] = PARTITION_SECTORS.to_bytes(4, "little")
    boot_record[TABLE_OFFSET:TABLE_OFFSET + TABLE_ENTRY_BYTES] = entry


def main() -> None:
    if len(sys.argv) != 6:
        raise SystemExit(f"usage: {sys.argv[0]} IMAGE STAGE1 STAGE2 KERNEL INITRAMFS")
    output, stage1_path, stage2_path, kernel_path, initramfs_path = sys.argv[1:]

    root = pathlib.Path(__file__).resolve().parent.parent
    fat = load_fat_builder(root)

    stage1 = bytearray(read(stage1_path))
    stage2 = read(stage2_path)
    kernel = read(kernel_path)
    initramfs = read(initramfs_path)

    if len(stage1) != SECTOR_SIZE or stage1[510:512] != b"\x55\xaa":
        raise SystemExit("stage1 must be exactly 512 bytes with the 0xAA55 signature")
    if any(stage1[TABLE_OFFSET:SIGNATURE_OFFSET]):
        raise SystemExit("stage1 has grown into the partition table")
    write_partition_entry(stage1)

    stage2_sectors = math.ceil(len(stage2) / SECTOR_SIZE)
    if 1 + stage2_sectors > PARTITION_LBA:
        raise SystemExit(
            f"stage2 needs {stage2_sectors} sectors; the partition starts at "
            f"LBA {PARTITION_LBA}"
        )

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
        1,                       # stage2 lba
        stage2_sectors,
        PARTITION_LBA,           # the kernel is in the filesystem now
        0,
        len(kernel),
        INITRAMFS_LBA,
        initramfs_sectors,
        len(initramfs),
        crc32(kernel),
        crc32(initramfs),
    ).ljust(SECTOR_SIZE, b"\0")

    config = CONFIG_TEMPLATE.format(manifest_lba=MANIFEST_LBA).encode()

    # Built by the loader's own tool, so the two cannot drift apart. The FAT
    # has to grow with the partition: one entry per cluster, four bytes each.
    fat.PARTITION_SECTORS = PARTITION_SECTORS
    fat.SECTORS_PER_FAT = -(-(PARTITION_SECTORS + 2) * 4 // SECTOR_SIZE)
    volume = fat.build_fat32([(CONFIG_NAME, config), (KERNEL_NAME, kernel)])

    image = bytearray(PARTITION_LBA * SECTOR_SIZE)
    image[0:SECTOR_SIZE] = stage1
    image[SECTOR_SIZE:SECTOR_SIZE + len(stage2)] = stage2
    image += volume

    if len(image) != MANIFEST_LBA * SECTOR_SIZE:
        raise SystemExit(
            f"the partition ended at LBA {len(image) // SECTOR_SIZE}, not "
            f"{MANIFEST_LBA}"
        )

    image += manifest
    image += initramfs
    image += b"\0" * (-len(image) % SECTOR_SIZE)

    # The ext2 region the kernel formats, starting on an aligned sector so the
    # driver's block arithmetic lines up with the disk's.
    used_sectors = len(image) // SECTOR_SIZE
    data_lba = -(-used_sectors // DATA_REGION_ALIGN_SECTORS) * DATA_REGION_ALIGN_SECTORS
    image += b"\0" * (data_lba * SECTOR_SIZE - len(image))

    # The data region is left as a hole. Building it in memory would mean
    # holding the whole reservation as bytes -- and then a second copy to
    # write it -- which stops being possible long before the region is large
    # enough to be useful. Extending the file instead costs nothing until the
    # guest writes, and the guest sees the same all-zero disk either way.
    total_bytes = len(image) + DATA_REGION_BYTES
    with open(output, "wb") as handle:
        handle.write(bytes(image))
        handle.truncate(total_bytes)

    print(f"image: {output} ({total_bytes} bytes)")
    print(f"stage2: {stage2_sectors} sectors")
    print(f"kernel: {len(kernel)} bytes in the partition crc32={crc32(kernel):08x}")
    print(f"manifest: lba {MANIFEST_LBA}")
    print(f"initramfs: {len(initramfs)} bytes/{initramfs_sectors} sectors "
          f"crc32={crc32(initramfs):08x}")
    print(f"ext2 data region: lba {data_lba}, "
          f"{DATA_REGION_BYTES // (1024 * 1024)} MiB")


if __name__ == "__main__":
    main()
