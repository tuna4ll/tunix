#!/bin/sh
set -eu

kernel="${1:-build/kernel.elf}"
limine="${2:-build/limine}"
arch="${ARCH:-x86_64}"
controller="${USB:-ehci}"
[ "$arch" = aarch64 ] && controller=xhci
work="$(mktemp -d /tmp/tunix-iolatency.XXXXXX)"
trap 'rm -rf "$work"' EXIT

truncate -s 96M "$work/stick.img"
mkfs.ext3 -q -b 4096 -L stick "$work/stick.img"

drive="-drive if=none,id=stick,format=raw,file=$work/stick.img"
drive="$drive,throttling.bps-write=${STICK_WRITE_BPS:-2097152},throttling.iops-write=${STICK_WRITE_IOPS:-60}"
if [ "$controller" = xhci ]; then
    usb="-device qemu-xhci,id=usb -device usb-storage,bus=usb.0,drive=stick"
else
    usb="-device usb-ehci,id=usb -device usb-storage,bus=usb.0,drive=stick"
fi

extra=
[ "$arch" = aarch64 ] && extra=-mno-outline-atomics
CPUS="${CPUS:-4}" QEMU_EXTRA="$drive $usb" CFLAGS_EXTRA="$extra" SHOW=IOLAT WAIT="${WAIT:-600}" \
    support/tests/kerneltest.sh support/tests/iolatency-kerneltest.c IOLAT "$kernel" "$limine"
e2fsck -fn "$work/stick.img" > "$work/fsck" 2>&1 || { cat "$work/fsck"; echo "IOLAT FAIL e2fsck"; exit 1; }
echo "IOLAT e2fsck clean"
