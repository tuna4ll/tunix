#!/bin/sh
set -eu

source="$1"
marker="$2"
kernel="$3"
limine="$4"
arch="${ARCH:-x86_64}"
show="${SHOW:-$marker}"
wait="${WAIT:-120}"
work="$(mktemp -d /tmp/tunix-kerneltest.XXXXXX)"
trap 'rm -rf "$work"' EXIT

test -f "$kernel"
mkdir -p "$work/root/sbin" "$work/root/proc" "$work/root/dev" "$work/root/tmp"
mkdir -p "$work/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png \
    "$work/root/usr/share/weston/wallpapers/tunix.png"

compiler=cc
flags="${CFLAGS_EXTRA:-}"
if [ "$arch" = aarch64 ]; then
    compiler=aarch64-linux-gnu-gcc
else
    test -x "$limine/limine"
    flags="$flags ${X86_CFLAGS:-}"
fi
$compiler -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables $flags "$source" -o "$work/root/sbin/init"

{
    echo 'timeout: 0'
    echo 'serial: yes'
    echo '/Tunix'
    echo '    protocol: limine'
    echo '    path: boot():/boot/kernel.elf'
    echo '    cmdline: root=LABEL=tunix-root'
} > "$work/limine.conf"

ARCH="$arch" TABLE=gpt ROOT_SLACK_MIB=16 support/image.sh "$work/tunix.img" "$kernel" \
    "$limine" "$work/limine.conf" "$work/root" >/dev/null

if [ "$arch" = aarch64 ]; then
    timeout "$wait" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 \
        -smp "${CPUS:-1}" -m "${MEMORY:-4G}" -kernel "$kernel" -append root=LABEL=tunix-root \
        -drive "format=raw,file=$work/tunix.img,if=none,id=disk0" \
        -device nvme,drive=disk0,serial=tunix -display none -no-reboot \
        -serial "file:$work/serial.log" >/dev/null 2>&1 &
else
    accel=tcg
    cpu=max
    if test -w /dev/kvm; then
        accel=kvm
        cpu=host
    fi
    timeout "$wait" qemu-system-x86_64 -machine "q35,accel=$accel" -cpu "$cpu" \
        -smp "${CPUS:-1}" -m "${MEMORY:-4G}" -drive "format=raw,file=$work/tunix.img,if=none,id=disk0" \
        -device ide-hd,drive=disk0,bus=ide.0 -display none -no-reboot \
        -serial "file:$work/serial.log" >/dev/null 2>&1 &
fi
qemu_pid=$!

for unused in $(seq "$wait"); do
    grep -aq "^$marker \(PASS\|FAIL\)" "$work/serial.log" 2>/dev/null && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 1
done
sleep 1
kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
if ! grep -a "^$show" "$work/serial.log"; then
    tail -80 "$work/serial.log"
    exit 1
fi
grep -aq "^$marker PASS" "$work/serial.log"
