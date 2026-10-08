#!/bin/sh
set -eu

kernel="${1:-build/kernel.elf}"
limine="${2:-build/limine}"
controller="${USB:-ehci}"
work="$(mktemp -d /tmp/tunix-usbread.XXXXXX)"
trap 'rm -rf "$work"' EXIT

head -c $((24 * 1024 * 1024)) /dev/urandom > "$work/data"
python3 - "$work/data" "$work/sum" <<'PY'
import sys
value = 1469598103934665603
for byte in open(sys.argv[1], 'rb').read():
    value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
open(sys.argv[2], 'w').write('%x\n' % value)
PY
truncate -s 64M "$work/stick.img"
mkfs.ext3 -q -b 4096 -L stick "$work/stick.img"
debugfs -w -R "write $work/data data" "$work/stick.img" >/dev/null 2>&1
debugfs -w -R "write $work/sum sum" "$work/stick.img" >/dev/null 2>&1

drive="-drive if=none,id=stick,format=raw,file=$work/stick.img"
if [ "$controller" = xhci ]; then
    usb="-device qemu-xhci,id=usb -device usb-storage,bus=usb.0,drive=stick"
else
    usb="-device usb-ehci,id=usb -device usb-storage,bus=usb.0,drive=stick"
fi
CPUS="${CPUS:-2}" QEMU_EXTRA="$drive $usb" SHOW=USBREAD WAIT="${WAIT:-300}" \
    support/tests/kerneltest.sh support/tests/usbread-kerneltest.c USBREAD "$kernel" "$limine"
