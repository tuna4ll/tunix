#!/bin/bash

KERNEL=${1:-build/kernel.elf}
ARCH=${ARCH:-x86_64}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=${LIMINE_DIR:-$BUILD/limine}
WORK=$BUILD/usbtest
IMAGE=$BUILD/usbtest.img
STICK=$BUILD/usbtest-stick.img
STICK2=$BUILD/usbtest-stick2.img
STICK3=$BUILD/usbtest-stick3.img
LOG=$BUILD/usbtest.log
MONITOR=$(mktemp -u /tmp/tunix-usbtest.XXXXXX)
WAIT=${WAIT:-300}
XHCI=${XHCI:-qemu-xhci}
CC=cc
[ "$ARCH" = aarch64 ] && CC="aarch64-linux-gnu-gcc -mno-outline-atomics"

[ -f "$KERNEL" ] || { echo "usbtest: $KERNEL is missing" >&2; exit 1; }
[ "$ARCH" != x86_64 ] || [ -x "$LIMINE_DIR/limine" ] || { echo "usbtest: limine is missing" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/root/sbin" "$WORK/root/dev" "$WORK/root/proc" "$WORK/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png "$WORK/root/usr/share/weston/wallpapers/tunix.png"
$CC -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	-fno-tree-loop-distribute-patterns -Isupport/tests \
	support/tests/usbtest.c -o "$WORK/root/sbin/init" || exit 1

cat > "$WORK/limine.conf" <<CONF
timeout: 0
serial: yes

/Tunix
    protocol: limine
    path: boot():/boot/kernel.elf
    cmdline: root=LABEL=tunix-root ${EXTRA_CMDLINE:-}
CONF

ARCH=$ARCH TABLE=gpt ROOT_SLACK_MIB=16 \
	support/image.sh "$IMAGE" "$KERNEL" "$LIMINE_DIR" "$WORK/limine.conf" "$WORK/root" >/dev/null || exit 1
rm -f "$STICK" "$STICK2" "$STICK3"
truncate -s 64M "$STICK" "$STICK2" "$STICK3"

MODE=${MODE:-xhci}
USB="-device $XHCI,id=xhci -device $XHCI,id=xhci2 \
	-device usb-hub,bus=xhci.0,port=1,id=hub1 \
	-device usb-kbd,bus=xhci.0,port=1.1,id=kbd1 \
	-device usb-mouse,bus=xhci.0,port=1.2,id=mouse1 \
	-drive if=none,id=stick,file=$STICK,format=raw \
	-device usb-storage,bus=xhci2.0,port=2,drive=stick,id=stick1 \
	-drive if=none,id=stick2,file=$STICK2,format=raw \
	-device usb-storage,bus=xhci.0,port=1.4,drive=stick2,id=stickhub \
	-drive if=none,id=stick3,file=$STICK3,format=raw"
if [ "$MODE" = ehci ]; then
	USB="-device usb-ehci,id=ehci \
	-device usb-kbd,bus=ehci.0,port=1,id=kbd1 \
	-device usb-mouse,bus=ehci.0,port=2,id=mouse1 \
	-drive if=none,id=stick,file=$STICK,format=raw \
	-device usb-storage,bus=ehci.0,port=3,drive=stick,id=stick1 \
	-drive if=none,id=stick2,file=$STICK2,format=raw \
	-device usb-storage,bus=ehci.0,port=4,drive=stick2,id=stick2dev"
fi

rm -f "$LOG" "$MONITOR"
if [ "$ARCH" = aarch64 ]; then
	timeout "$WAIT" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -smp ${SMP:-4} -m 2G \
		-kernel "$KERNEL" -append "root=LABEL=tunix-root ${EXTRA_CMDLINE:-}" \
		-drive "format=raw,file=$IMAGE,if=none,id=disk0" -device nvme,drive=disk0,serial=tunix \
		$USB -display none -no-reboot -serial "file:$LOG" \
		-qmp "unix:$MONITOR,server,nowait" >"$BUILD/usbtest-qemu.err" 2>&1 &
else
	ACCEL=tcg
	[ -w /dev/kvm ] && ACCEL=kvm
	timeout "$WAIT" qemu-system-x86_64 -machine "q35,accel=$ACCEL,i8042=off" -cpu host -smp ${SMP:-4} -m 2G \
		-drive "format=raw,file=$IMAGE,if=none,id=disk0" -device ide-hd,drive=disk0,bus=ide.0 \
		$USB -display none -no-reboot -serial "file:$LOG" \
		-qmp "unix:$MONITOR,server,nowait" >"$BUILD/usbtest-qemu.err" 2>&1 &
fi
QEMU=$!

for _ in $(seq "$WAIT"); do
	grep -aq "USBTEST READY" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done

python3 - "$MONITOR" "${HOTDISK:-1}" "$MODE" <<'PY'
import json, socket, sys, time
hot = sys.argv[2] == '1'
ehci = sys.argv[3] == 'ehci'
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(sys.argv[1])
stream = sock.makefile('rw')
def send(message):
    stream.write(json.dumps(message) + '\n'); stream.flush()
    while True:
        reply = json.loads(stream.readline())
        if 'return' in reply or 'error' in reply:
            if 'error' in reply: print('usbtest: qmp', message.get('execute'), reply['error'])
            return reply
json.loads(stream.readline())
send({'execute': 'qmp_capabilities'})
def key(device, name, times):
    for _ in range(times):
        for down in (True, False):
            send({'execute': 'input-send-event', 'arguments': {'events': [
                {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]}})
            time.sleep(0.04)
def mouse(device, times):
    for _ in range(times):
        send({'execute': 'input-send-event', 'arguments': {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': 5}}]}})
        time.sleep(0.04)
    for down in (True, False):
        send({'execute': 'input-send-event', 'arguments': {'events': [
            {'type': 'btn', 'data': {'down': down, 'button': 'left'}}]}})
        time.sleep(0.1)
time.sleep(2)
key('kbd1', 'a', 10)
mouse('mouse1', 10)
if ehci:
    key('kbd1', 'd', 30)
    mouse('mouse1', 10)
    sys.exit(0)
send({'execute': 'device_del', 'arguments': {'id': 'kbd1'}})
time.sleep(4)
send({'execute': 'device_add', 'arguments': {'driver': 'usb-kbd', 'id': 'kbd2', 'bus': 'xhci2.0', 'port': '1'}})
time.sleep(4)
key('kbd2', 'b', 10)
send({'execute': 'device_add', 'arguments': {'driver': 'usb-kbd', 'id': 'kbd3', 'bus': 'xhci.0', 'port': '1.3'}})
time.sleep(4)
key('kbd3', 'c', 10)
key('kbd2', 'd', 30)
if hot: send({'execute': 'device_add', 'arguments': {'driver': 'usb-storage', 'id': 'hot', 'drive': 'stick3', 'bus': 'xhci2.0', 'port': '3'}})
time.sleep(10)
if hot: send({'execute': 'device_del', 'arguments': {'id': 'hot'}})
time.sleep(6)
key('kbd2', 'e', 10)
send({'execute': 'device_del', 'arguments': {'id': 'mouse1'}})
time.sleep(3)
send({'execute': 'device_add', 'arguments': {'driver': 'usb-mouse', 'id': 'mouse2', 'bus': 'xhci2.0', 'port': '4'}})
time.sleep(4)
mouse('mouse2', 10)
PY

for _ in $(seq 120); do
	grep -aq "USBTEST DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true
rm -f "$MONITOR"

grep -aE "^(XHCI|EHCI|USB|BLOCK: sd[b-z])" "$LOG"
grep -aq "USBTEST DONE" "$LOG" || { echo "usbtest: no result; $LOG has the boot" >&2; exit 1; }
python3 - "$LOG" "${HOTDISK:-1}" "$MODE" <<'PY'
import re, sys
text = open(sys.argv[1], errors='replace').read()
failures = []
ehci = sys.argv[3] == 'ehci'
for code, name in (((30, 'a'), (32, 'd')) if ehci else ((30, 'a'), (48, 'b'), (46, 'c'), (32, 'd'), (18, 'e'))):
    match = re.search(r'USBKEY code=%d press=(\d+) release=(\d+)' % code, text)
    if not match or match.group(1) != ('30' if name == 'd' else '10') or match.group(1) != match.group(2):
        failures.append('key ' + name)
mouse = re.search(r'USBMOUSE rel=(\d+) press=(\d+) release=(\d+)', text)
if not mouse or int(mouse.group(1)) < 20 or mouse.group(2) != '2' or mouse.group(3) != '2':
    failures.append('mouse')
disks = ['/dev/sdb', '/dev/sdc'] + (['/dev/sdd'] if sys.argv[2] == '1' and not ehci else [])
for disk in disks:
    match = re.search(r'USBDISK %s ok=(\d+) bad=(\d+) errors=(\d+)' % disk, text)
    if not match or int(match.group(1)) == 0 or match.group(2) != '0':
        failures.append(disk)
    elif disk != '/dev/sdd' and match.group(3) != '0':
        failures.append(disk + ' errors')
if 'PANIC' in text or 'KERNEL EXCEPTION' in text:
    failures.append('panic')
print('USBTEST ' + ('PASS' if not failures else 'FAIL ' + ' '.join(failures)))
sys.exit(1 if failures else 0)
PY
