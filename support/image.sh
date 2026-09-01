#!/bin/bash
#
# Build the disk image: a GPT disk with an EFI system partition Limine boots
# from and an ext2 root the kernel mounts.
#
# One image boots both firmwares. UEFI runs EFI/BOOT/BOOTX64.EFI off the ESP;
# BIOS runs the stage written into the partition table's boot record by
# `limine bios-install`, which then finds limine-bios.sys on the ESP.
#
# TABLE picks the partition scheme. GPT is the default and is what a UEFI
# machine expects. MBR exists for old BIOS machines booting from a USB stick:
# their firmware decides between hard-disk and floppy emulation by looking for
# an active partition in the boot record, a GPT disk's protective MBR has none,
# and in floppy emulation the loader is handed a device with no partition table
# to search -- which it reports as `Stage 3 file not found`.
set -euo pipefail

IMAGE=${1:?usage: image.sh IMAGE KERNEL LIMINE_DIR LIMINE_CONF SYSROOT}
KERNEL=${2:?}
LIMINE_DIR=${3:?}
LIMINE_CONF=${4:?}
SYSROOT=${5:?}

TABLE=${TABLE:-gpt}
case $TABLE in
gpt | mbr) ;;
*) echo "image.sh: TABLE must be gpt or mbr, not $TABLE" >&2; exit 1 ;;
esac

ESP_MIB=${ESP_MIB:-64}
# Headroom over what the tree actually needs, so that the machine has somewhere
# to put what it installs later. Four gigabytes rather than the half it used to
# be because "a package" turned out to mean things like a Qt application with a
# JVM under it, which arrive with hundreds of megabytes of their own and then
# download more. Nothing is written that is not used, so the cost is the time
# to build the image and to copy it to a stick.
ROOT_SLACK_MIB=${ROOT_SLACK_MIB:-4096}

WORK=$(dirname "$IMAGE")/image
rm -rf "$WORK"
mkdir -p "$WORK"

# --- the EFI system partition ----------------------------------------------

#
# One directory per mmd, because the way a later copy goes wrong is unreadable.
# mcopy answers a *missing target directory* with
#
#   ::/boot/limine/limine-bios.sys: no match for target
#   Bad target ::/boot/limine/limine-bios.sys
#
# which names the file it was asked to write and says nothing about the
# directory that is actually absent. With `set -e`, mmd itself is the reliable
# check: probing an empty directory afterward has produced false negatives in
# released mtools versions even though the directory was created successfully.
esp_mkdir() {
	mmd -i "$WORK/esp.img" "$1"
}

# And a source that is not there is worth its own sentence: mcopy reports it as
# a plain "No such file or directory", which reads like a bug in the image
# rather than a download that did not finish.
esp_copy() {
	[ -f "$1" ] || { echo "image.sh: $1 is missing." >&2; exit 1; }
	mcopy -i "$WORK/esp.img" "$1" "$2"
}

echo ":: building the ESP"
truncate -s "${ESP_MIB}M" "$WORK/esp.img"
mformat -i "$WORK/esp.img" -F -v TUNIX ::
esp_mkdir ::/EFI
esp_mkdir ::/EFI/BOOT
esp_mkdir ::/boot
esp_mkdir ::/boot/limine
esp_copy "$LIMINE_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
esp_copy "$LIMINE_DIR/limine-bios.sys" ::/boot/limine/limine-bios.sys
esp_copy "$LIMINE_CONF" ::/boot/limine/limine.conf
esp_copy "$KERNEL" ::/boot/kernel.elf

# --- the root filesystem ----------------------------------------------------
#
# The feature set is not the default one. The kernel's ext2 driver reads
# classic ext2 and nothing else: 4 KiB blocks, 128-byte inodes, no extents, no
# 64-bit block numbers, no checksummed metadata, and no hashed directories --
# it would have to maintain the hash tree to write into one. mke2fs is happy to
# leave all of that out; see superblock_usable() in kernel/fs/ext2.c.
ROOT_MIB=$(( $(du -sm "$SYSROOT" | cut -f1) + ROOT_SLACK_MIB ))
echo ":: building a ${ROOT_MIB} MiB root filesystem"
truncate -s "${ROOT_MIB}M" "$WORK/root.img"
mkfs.ext2 -q -b 4096 -I 128 -m 1 -L tunix-root \
	-O ^resize_inode,^dir_index,^ext_attr,^metadata_csum,^64bit,^huge_file,^dir_nlink,^extra_isize \
	-d "$SYSROOT" "$WORK/root.img"
# mke2fs leaves the filesystem marked "not cleanly unmounted" after -d on some
# versions; the kernel refuses anything but a clean superblock, and e2fsck is
# the thing that says so authoritatively.
e2fsck -fp "$WORK/root.img" >/dev/null || [ $? -lt 4 ]

# --- the disk ---------------------------------------------------------------

ESP_SECTORS=$(( ESP_MIB * 1024 * 1024 / 512 ))
ROOT_SECTORS=$(( ROOT_MIB * 1024 * 1024 / 512 ))
ESP_START=2048
ROOT_START=$(( ESP_START + ESP_SECTORS ))
# 2048 sectors of slack at the end for the backup GPT.
TOTAL_SECTORS=$(( ROOT_START + ROOT_SECTORS + 2048 ))

echo ":: writing $IMAGE"
rm -f "$IMAGE"
truncate -s $(( TOTAL_SECTORS * 512 )) "$IMAGE"
if [ "$TABLE" = gpt ]; then
	sfdisk --quiet --label gpt "$IMAGE" <<EOF
start=$ESP_START, size=$ESP_SECTORS, type=uefi, name="EFI System"
start=$ROOT_START, size=$ROOT_SECTORS, type=linux, name="tunix-root"
EOF
else
	# `bootable` is the whole point: it is the flag an old BIOS looks for.
	sfdisk --quiet --label dos "$IMAGE" <<EOF
start=$ESP_START, size=$ESP_SECTORS, type=ef, bootable
start=$ROOT_START, size=$ROOT_SECTORS, type=83
EOF
fi

# seek_bytes so the block size can be chosen for throughput rather than to make
# the offset land on a whole number of blocks. At 512 bytes this took minutes.
dd if="$WORK/esp.img" of="$IMAGE" bs=4M oflag=seek_bytes \
	seek=$(( ESP_START * 512 )) conv=notrunc status=none
dd if="$WORK/root.img" of="$IMAGE" bs=4M oflag=seek_bytes \
	seek=$(( ROOT_START * 512 )) conv=notrunc status=none

# BIOS: the first stage goes in the gap between the boot record and the first
# partition, which is why the ESP starts at sector 2048 rather than 34. On a
# GPT disk Limine has to split the second stage around the partition entry
# array; on an MBR one the gap is free and it stays in one piece.
"$LIMINE_DIR/limine" bios-install "$IMAGE"

rm -rf "$WORK"
echo ":: $IMAGE ready, $TABLE ($(du -h "$IMAGE" | cut -f1))"
