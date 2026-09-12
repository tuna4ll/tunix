#!/bin/bash
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
ROOT_SLACK_MIB=${ROOT_SLACK_MIB:-4096}

WORK=$(dirname "$IMAGE")/image
rm -rf "$WORK"
mkdir -p "$WORK"

esp_copy() {
	[ -f "$1" ] || { echo "image.sh: $1 is missing." >&2; exit 1; }
	cp "$1" "$WORK/esp-root/$2"
}

echo ":: building the ESP"
truncate -s "${ESP_MIB}M" "$WORK/esp.img"
mformat -i "$WORK/esp.img" -F -v TUNIX ::
mkdir -p "$WORK/esp-root/EFI/BOOT" "$WORK/esp-root/boot/limine"
esp_copy "$LIMINE_DIR/BOOTX64.EFI" EFI/BOOT/BOOTX64.EFI
esp_copy "$LIMINE_DIR/limine-bios.sys" boot/limine/limine-bios.sys
esp_copy "$LIMINE_CONF" boot/limine/limine.conf
esp_copy "$KERNEL" boot/kernel.elf
esp_copy "$SYSROOT/usr/share/weston/wallpapers/tunix.png" boot/wallpaper.png
mcopy -s -i "$WORK/esp.img" "$WORK/esp-root"/* ::

ROOT_MIB=$(( $(du -sm "$SYSROOT" | cut -f1) + ROOT_SLACK_MIB ))
echo ":: building a ${ROOT_MIB} MiB root filesystem"
truncate -s "${ROOT_MIB}M" "$WORK/root.img"
mkfs.ext3 -q -r 1 -b 4096 -I 128 -m 1 -L tunix-root \
	-O ^resize_inode,^dir_index,^ext_attr,^metadata_csum,^64bit,^huge_file,^dir_nlink,^extra_isize \
	-d "$SYSROOT" "$WORK/root.img"
e2fsck -fp "$WORK/root.img" >/dev/null || [ $? -lt 4 ]

ESP_SECTORS=$(( ESP_MIB * 1024 * 1024 / 512 ))
ROOT_SECTORS=$(( ROOT_MIB * 1024 * 1024 / 512 ))
ESP_START=2048
ROOT_START=$(( ESP_START + ESP_SECTORS ))
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
	sfdisk --quiet --label dos "$IMAGE" <<EOF
start=$ESP_START, size=$ESP_SECTORS, type=ef, bootable
start=$ROOT_START, size=$ROOT_SECTORS, type=83
EOF
fi

dd if="$WORK/esp.img" of="$IMAGE" bs=4M oflag=seek_bytes \
	seek=$(( ESP_START * 512 )) conv=notrunc status=none
dd if="$WORK/root.img" of="$IMAGE" bs=4M oflag=seek_bytes \
	seek=$(( ROOT_START * 512 )) conv=notrunc status=none

"$LIMINE_DIR/limine" bios-install "$IMAGE"

rm -rf "$WORK"
if [ -n "${SUDO_UID:-}" ] && [ -n "${SUDO_GID:-}" ]; then
	chown "$SUDO_UID:$SUDO_GID" "$IMAGE"
fi
echo ":: $IMAGE ready, $TABLE ($(du -h "$IMAGE" | cut -f1))"
