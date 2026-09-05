#!/bin/bash
# Type into the machine while several processors read the keyboard, and count
# what came out: one press and one release per key, or the bug is still there.

CPUS=${1:-4}
KERNEL=${2:-build/kernel.elf}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=$BUILD/limine
WORK=$BUILD/inputtest
IMAGE=$BUILD/inputtest-$CPUS.img
LOG=$BUILD/inputtest-$CPUS.log
MONITOR=$BUILD/inputtest-$CPUS.mon
KEYS=${KEYS:-40}

[ -f "$KERNEL" ] || { echo "inputtest: $KERNEL is missing; run make kernel" >&2; exit 1; }
[ -x "$LIMINE_DIR/limine" ] || { echo "inputtest: limine is missing; run make kernel" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/root/sbin" "$WORK/root/dev" "$WORK/root/proc"

cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	support/tests/inputtest.c -o "$WORK/root/sbin/init"

cat > "$WORK/limine.conf" <<CONF
timeout: 0
serial: yes

/Tunix
    protocol: limine
    path: boot():/boot/kernel.elf
    cmdline: root=LABEL=tunix-root ${EXTRA_CMDLINE:-}
CONF

TABLE=${IMAGE_TABLE:-gpt} ROOT_SLACK_MIB=16 \
	support/image.sh "$IMAGE" "$KERNEL" "$LIMINE_DIR" "$WORK/limine.conf" "$WORK/root" >/dev/null

if [ "${BOOT:-1}" = 0 ]; then
	echo ":: $IMAGE ready -- write it with: sudo dd if=$IMAGE of=/dev/sdX bs=4M oflag=direct status=progress"
	exit 0
fi

ACCEL=tcg
[ -w /dev/kvm ] && ACCEL=kvm

echo ":: booting $CPUS-processor machine on $ACCEL"
rm -f "$LOG" "$MONITOR"
timeout 180 qemu-system-x86_64 \
	-machine "q35,accel=$ACCEL" -cpu host -smp "$CPUS" -m 4G \
	-drive "format=raw,file=$IMAGE,if=none,id=disk0" \
	-device ide-hd,drive=disk0,bus=ide.0 \
	-display none -no-reboot -serial "file:$LOG" \
	-monitor "unix:$MONITOR,server,nowait" >/dev/null 2>&1 &
QEMU=$!

for _ in $(seq 120); do
	grep -q "INPUTTEST READY" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done

# The keys, typed one at a time down the monitor socket. `sendkey` is a press
# and a release, so the machine should report one of each per call.
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

for _ in $(seq 60); do
	grep -q "INPUTTEST DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true
rm -f "$MONITOR"

echo ":: $KEYS keys sent"
grep -E "^(INPUT|INPUTTEST)" "$LOG" || {
	echo "inputtest: the machine printed no results; $LOG has the boot" >&2
	exit 1
}
