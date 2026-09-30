#!/bin/sh
set -eu

kernel="${1:-build/kernel.elf}"
limine="${2:-build/limine}"
arch="${ARCH:-x86_64}"
disks="$(mktemp -d /tmp/tunix-ext2test.XXXXXX)"
trap 'rm -rf "$disks"' EXIT

make_volume() {
    label="$1"
    shift
    seed="$disks/$label.d"
    mkdir -p "$seed/big"
    printf '%s\n' "$label" > "$seed/label"
    printf 'seed-%s\n' "$label" > "$seed/seed.txt"
    for index in $(seq 0 399); do : > "$seed/big/f$index"; done
    printf 'attributes\n' > "$seed/attr.txt"
    setfattr -n user.tag -v "$(printf '%0200d' 7)" "$seed/attr.txt"
    truncate -s 64M "$disks/$label.img"
    "$@" -q -L "$label" -d "$seed" "$disks/$label.img"
    e2fsck -fyD "$disks/$label.img" >/dev/null 2>&1 || [ $? -lt 4 ]
}

make_volume vol1k mkfs.ext2 -b 1024 -I 256
make_volume vol2k mkfs.ext3 -b 2048 -I 256
make_volume vol4k mkfs.ext2 -b 4096 -I 128

extra=""
number=1
for label in vol1k vol2k vol4k; do
    drive="-drive format=raw,file=$disks/$label.img,if=none,id=extra$number"
    if [ "$arch" = aarch64 ]; then
        extra="$extra $drive -device nvme,drive=extra$number,serial=extra$number"
    else
        extra="$extra $drive -device ide-hd,drive=extra$number,bus=ide.$number"
    fi
    number=$((number + 1))
done

QEMU_EXTRA="$extra" SHOW=EXT2 WAIT="${WAIT:-240}" \
    support/tests/kerneltest.sh support/tests/ext2-kerneltest.c EXT2TEST "$kernel" "$limine"

status=0
for label in vol1k vol2k vol4k; do
    if e2fsck -fn "$disks/$label.img" > "$disks/$label.fsck" 2>&1; then
        echo "EXT2HOST ok $label e2fsck clean"
    else
        echo "EXT2HOST FAIL $label e2fsck"
        cat "$disks/$label.fsck"
        status=1
    fi
done
debugfs -R "cat /moved" "$disks/vol2k.img" 2>/dev/null > "$disks/moved"
if [ "$(stat -c %s "$disks/moved")" = 70000 ]; then
    echo "EXT2HOST ok moved file readable by debugfs"
else
    echo "EXT2HOST FAIL moved file"
    status=1
fi
size="$(debugfs -R "stat /far" "$disks/vol4k.img" 2>/dev/null | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)"
if [ "$size" = $((5 * 1024 * 1024 * 1024 + 8)) ]; then
    echo "EXT2HOST ok large file size $size"
else
    echo "EXT2HOST FAIL large file size $size"
    status=1
fi
exit $status
