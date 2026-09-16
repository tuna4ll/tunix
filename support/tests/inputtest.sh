#!/bin/bash

CPUS=${1:-4}
KERNEL=${2:-build/kernel.elf}
ARCH=${ARCH:-x86_64}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=${LIMINE_DIR:-$BUILD/limine}
WORK=$BUILD/inputtest
IMAGE=$BUILD/inputtest-$CPUS.img
LOG=$BUILD/inputtest-$CPUS.log
WAIT=${WAIT:-180}
MONITOR=$(mktemp -u /tmp/tunix-inputtest.XXXXXX)
KEYS=${KEYS:-40}
CC=cc
[ "$ARCH" = aarch64 ] && CC="aarch64-linux-gnu-gcc -mno-outline-atomics"

[ -f "$KERNEL" ] || { echo "inputtest: $KERNEL is missing; run make kernel" >&2; exit 1; }
[ "$ARCH" != x86_64 ] || [ -x "$LIMINE_DIR/limine" ] || { echo "inputtest: limine is missing; run make kernel" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/root/sbin" "$WORK/root/dev" "$WORK/root/proc"
mkdir -p "$WORK/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png "$WORK/root/usr/share/weston/wallpapers/tunix.png"

$CC -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	support/tests/inputtest.c -o "$WORK/root/sbin/init" || exit 1

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

rm -f "$LOG" "$MONITOR"
if [ "$ARCH" = aarch64 ]; then
	echo ":: booting $CPUS-processor aarch64 machine on tcg"
	timeout "$WAIT" qemu-system-aarch64 \
		-M virt,gic-version=3 -cpu cortex-a72 -smp "$CPUS" -m ${BENCH_MEMORY:-4G} \
		-kernel "$KERNEL" -append "root=LABEL=tunix-root ${EXTRA_CMDLINE:-}" \
		-device qemu-xhci -device usb-kbd -drive "format=raw,file=$IMAGE,if=none,id=disk0" \
		-device nvme,drive=disk0,serial=tunix \
		-display none -no-reboot -serial "file:$LOG" \
		-monitor "unix:$MONITOR,server,nowait" >/dev/null 2>&1 &
else
	ACCEL=tcg
	[ -w /dev/kvm ] && ACCEL=kvm
	echo ":: booting $CPUS-processor machine on $ACCEL"
	timeout "$WAIT" qemu-system-x86_64 \
		-machine "q35,accel=$ACCEL" -cpu host -smp "$CPUS" -m ${BENCH_MEMORY:-4G} \
		-drive "format=raw,file=$IMAGE,if=none,id=disk0" \
		-device ide-hd,drive=disk0,bus=ide.0 \
		-display none -no-reboot -serial "file:$LOG" \
		-monitor "unix:$MONITOR,server,nowait" >/dev/null 2>&1 &
fi
QEMU=$!

for _ in $(seq "$WAIT"); do
	grep -aq "INPUTTEST READY" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done

python3 - "$MONITOR" "$KEYS" <<'PY'
import socket, sys, time
path, count = sys.argv[1], int(sys.argv[2])
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(path)
time.sleep(0.5)
sock.recv(65536)
for _ in range(count):
    sock.sendall(b"sendkey a\n")
    time.sleep(0.05)
    try:
        sock.recv(65536)
    except BlockingIOError:
        pass
sock.close()
PY

for _ in $(seq 120); do
	grep -aq "INPUTTEST DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true
rm -f "$MONITOR"

echo ":: $KEYS keys sent"
grep -aE "^(INPUT|INPUTTEST)" "$LOG" || {
	echo "inputtest: the machine printed no results; $LOG has the boot" >&2
	exit 1
}
