#!/bin/sh
set -eu

kernel="${1:-build/kernel.elf}"
limine="${2:-build/limine}"
work="$(mktemp -d /tmp/tunix-eventfs-kerneltest.XXXXXX)"
trap 'rm -rf "$work"' EXIT

test -f "$kernel"
test -x "$limine/limine"
mkdir -p "$work/root/sbin" "$work/root/tmp" "$work/root/dev" "$work/root/proc"
mkdir -p "$work/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png \
    "$work/root/usr/share/weston/wallpapers/tunix.png"
cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables support/tests/eventfs-kerneltest.c \
    -o "$work/root/sbin/init"

{
    echo 'timeout: 0'
    echo 'serial: yes'
    echo '/Tunix'
    echo '    protocol: limine'
    echo '    path: boot():/boot/kernel.elf'
    echo '    cmdline: root=LABEL=tunix-root'
} > "$work/limine.conf"

TABLE=gpt ROOT_SLACK_MIB=16 support/image.sh "$work/tunix.img" "$kernel" \
    "$limine" "$work/limine.conf" "$work/root" >/dev/null

accel=tcg
cpu=max
if test -w /dev/kvm; then
    accel=kvm
    cpu=host
fi
timeout 120 qemu-system-x86_64 -machine "q35,accel=$accel" -cpu "$cpu" \
    -smp 4 -m 4G -drive "format=raw,file=$work/tunix.img,if=none,id=disk0" \
    -device ide-hd,drive=disk0,bus=ide.0 -display none -no-reboot \
    -serial "file:$work/serial.log" >/dev/null 2>&1 &
qemu_pid=$!

for unused in $(seq 120); do
    grep -q 'EVENTFSTEST DONE' "$work/serial.log" 2>/dev/null && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 1
done
kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
grep '^EVENTFS' "$work/serial.log"
grep -q '^EVENTFSTEST PASS$' "$work/serial.log"
