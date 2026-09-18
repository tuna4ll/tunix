#!/bin/bash

CPUS=${1:-4}
KERNEL=${2:-build/kernel.elf}
ARCH=${ARCH:-x86_64}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=${LIMINE_DIR:-$BUILD/limine}
WORK=$BUILD/drmtest
IMAGE=$BUILD/soundtest-$CPUS.img
LOG=$BUILD/soundtest-$CPUS.log
WAV=$BUILD/soundtest-$CPUS.wav
WAIT=${WAIT:-240}
CC=cc
[ "$ARCH" = aarch64 ] && CC=aarch64-linux-gnu-gcc

[ -f "$KERNEL" ] || { echo "soundtest: $KERNEL is missing; run make kernel" >&2; exit 1; }
[ "$ARCH" != x86_64 ] || [ -x "$LIMINE_DIR/limine" ] || { echo "soundtest: limine is missing; run make kernel" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/root/sbin" "$WORK/root/dev" "$WORK/root/proc"
mkdir -p "$WORK/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png "$WORK/root/usr/share/weston/wallpapers/tunix.png"

MODULE_DIR=$BUILD
[ "$ARCH" = aarch64 ] && MODULE_DIR=$BUILD/aarch64-core
mkdir -p "$WORK/root/modules"
cp "$MODULE_DIR/modules/snd_hda.ko" "$WORK/root/modules/" || exit 1

mkdir -p "$WORK/root/files"
for i in $(seq 0 399); do
	printf %s "$(head -c 8192 /dev/zero | tr "\\0" "x")" > "$WORK/root/files/f$(printf %03d $i)"
done

$CC -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	support/tests/soundtest.c -o "$WORK/root/sbin/init" || exit 1

cat > "$WORK/limine.conf" <<CONF
timeout: 0
serial: yes

/Tunix
    protocol: limine
    path: boot():/boot/kernel.elf
    cmdline: root=LABEL=tunix-root ${EXTRA_CMDLINE:-}
CONF

ARCH=$ARCH TABLE=${IMAGE_TABLE:-gpt} ROOT_SLACK_MIB=16 \
	support/image.sh "$IMAGE" "$KERNEL" "$LIMINE_DIR" "$WORK/limine.conf" "$WORK/root" >/dev/null || exit 1

if [ "${BOOT:-1}" = 0 ]; then
	echo ":: $IMAGE ready -- write it with: sudo dd if=$IMAGE of=/dev/sdX bs=4M oflag=direct status=progress"
	exit 0
fi

rm -f "$LOG" "$WAV"
SOUND="-audiodev wav,id=snd0,path=$WAV -device intel-hda -device hda-output,audiodev=snd0"
if [ "$ARCH" = aarch64 ]; then
	echo ":: booting $CPUS-processor aarch64 machine on tcg"
	timeout "$WAIT" qemu-system-aarch64 \
		-M virt,gic-version=3 -cpu cortex-a72 -smp "$CPUS" -m ${BENCH_MEMORY:-2G} \
		-kernel "$KERNEL" -append "root=LABEL=tunix-root ${EXTRA_CMDLINE:-}" \
		$SOUND -device ramfb -drive "format=raw,file=$IMAGE,if=none,id=disk0" \
		-device nvme,drive=disk0,serial=tunix \
		-display none -no-reboot -serial "file:$LOG" >/dev/null 2>&1 &
else
	ACCEL=tcg
	[ -w /dev/kvm ] && ACCEL=kvm
	echo ":: booting $CPUS-processor machine on $ACCEL"
	timeout "$WAIT" qemu-system-x86_64 \
		-machine "q35,accel=$ACCEL" -cpu host -smp "$CPUS" -m ${BENCH_MEMORY:-4G} \
		$SOUND -drive "format=raw,file=$IMAGE,if=none,id=disk0" \
		-device ide-hd,drive=disk0,bus=ide.0 \
		-display none -no-reboot -serial "file:$LOG" >/dev/null 2>&1 &
fi
QEMU=$!

for _ in $(seq "$WAIT"); do
	grep -q "SOUNDTEST DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true

[ -f "$WAV" ] && python3 support/tests/wavcheck.py "$WAV" || echo "WAV missing"

grep -E "^(SOUND|DF|SOUNDTEST)" "$LOG" || {
	echo "soundtest: the machine printed no results; $LOG has the boot" >&2
	exit 1
}
