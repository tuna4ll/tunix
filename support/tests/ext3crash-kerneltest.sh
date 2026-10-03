#!/bin/sh
set -eu

kernel="${1:-build/kernel.elf}"
limine="${2:-build/limine}"
arch="${ARCH:-x86_64}"
rounds="${ROUNDS:-3}"
work="$(mktemp -d /tmp/tunix-ext3crash.XXXXXX)"
trap 'rm -rf "$work"' EXIT

compiler=cc
flags="-Isupport/tests"
[ "$arch" = aarch64 ] && compiler="aarch64-linux-gnu-gcc -mno-outline-atomics"
mkdir -p "$work/root/sbin" "$work/root/proc" "$work/root/dev" "$work/root/tmp"
mkdir -p "$work/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png "$work/root/usr/share/weston/wallpapers/"
$compiler -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
    $flags support/tests/ext3crash-kerneltest.c -o "$work/root/sbin/init"
printf 'timeout: 0\nserial: yes\n/Tunix\n    protocol: limine\n    path: boot():/boot/kernel.elf\n    cmdline: root=LABEL=tunix-root\n' \
    > "$work/limine.conf"
ARCH="$arch" TABLE=gpt ROOT_SLACK_MIB=16 support/image.sh "$work/tunix.img" \
    "$kernel" "$limine" "$work/limine.conf" "$work/root" >/dev/null

truncate -s 64M "$work/crash.img"
mkfs.ext3 -q -b 1024 -L crash "$work/crash.img"

status=0
for round in $(seq "$rounds"); do
    : > "$work/serial.log"
    if [ "$arch" = aarch64 ]; then
        qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -smp 4 -m 2G \
            -kernel "$kernel" -append root=LABEL=tunix-root \
            -drive "format=raw,file=$work/tunix.img,if=none,id=disk0" \
            -device nvme,drive=disk0,serial=tunix \
            -drive "format=raw,file=$work/crash.img,if=none,id=disk1" \
            -device nvme,drive=disk1,serial=crash \
            -display none -no-reboot -serial "file:$work/serial.log" >/dev/null 2>&1 &
    else
        accel=tcg
        cpu=max
        test -w /dev/kvm && accel=kvm && cpu=host
        qemu-system-x86_64 -machine "q35,accel=$accel" -cpu "$cpu" -smp 4 -m 2G \
            -drive "format=raw,file=$work/tunix.img,if=none,id=disk0" \
            -device ide-hd,drive=disk0,bus=ide.0,bootindex=0 \
            -drive "format=raw,file=$work/crash.img,if=none,id=disk1" \
            -device ide-hd,drive=disk1,bus=ide.1 \
            -display none -no-reboot -serial "file:$work/serial.log" >/dev/null 2>&1 &
    fi
    qemu=$!
    for unused in $(seq 300); do
        grep -aq "^CRASH go\|^CRASH FAIL" "$work/serial.log" 2>/dev/null && break
        sleep 1
    done
    if grep -aq "^CRASH FAIL" "$work/serial.log" || ! grep -aq "^CRASH go" "$work/serial.log"; then
        kill -9 "$qemu" 2>/dev/null || true
        tail -30 "$work/serial.log"
        echo "EXT3CRASH FAIL round $round never started"
        exit 1
    fi
    sleep $((round * 3 + 2))
    kill -9 "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    synced=$(sed -n 's/^CRASH synced \([0-9][0-9]*\)$/\1/p' "$work/serial.log" | tail -1)
    grep -a "LOCK: cpu\|LOCK: pid\|LOCK: releases" "$work/serial.log" && status=1

    [ -n "${CRASH_DEBUG:-}" ] && dumpe2fs -h "$work/crash.img" 2>/dev/null | grep -i "features\|journal"
    e2fsck -fy "$work/crash.img" > "$work/replay.log" 2>&1 || [ $? -lt 4 ] || {
        cat "$work/replay.log"; echo "EXT3CRASH FAIL round $round replay"; exit 1; }
    if ! e2fsck -fn "$work/crash.img" > "$work/check.log" 2>&1; then
        cat "$work/replay.log" "$work/check.log"
        echo "EXT3CRASH FAIL round $round e2fsck after replay"
        exit 1
    fi
    if [ -n "$synced" ]; then
        for number in $(seq 0 "$synced"); do
            want=$(python3 -c "import sys;n=int(sys.argv[1]);sys.stdout.write(''.join(chr(97+(n*7+i)%26) for i in range(6000)))" "$number")
            got=$(debugfs -R "cat /kept/f$number" "$work/crash.img" 2>/dev/null)
            if [ "$got" != "$want" ]; then
                echo "EXT3CRASH FAIL round $round /kept/f$number lost after fsync"
                exit 1
            fi
        done
    fi
    echo "EXT3CRASH ok round $round synced=${synced:-none} replay=$(grep -aci "journal" "$work/replay.log")"
done
[ "$status" = 0 ] && echo "EXT3CRASH PASS" || { echo "EXT3CRASH FAIL lock warnings"; exit 1; }
