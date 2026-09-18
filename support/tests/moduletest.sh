#!/bin/bash

KERNEL=${1:-build/kernel.elf}
ARCH=${ARCH:-x86_64}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=${LIMINE_DIR:-$BUILD/limine}
RELEASE=$(sed -n 's/^#define UTS_RELEASE "\(.*\)"/\1/p' kernel/include/uts.h)
if [ "$ARCH" = aarch64 ]; then
	SYSROOT=${SYSROOT:-build/sysroot-aarch64}
	MODULE_DIR=${MODULE_DIR:-$BUILD/aarch64-core}
	CC="aarch64-linux-gnu-gcc -mno-outline-atomics"
else
	SYSROOT=${SYSROOT:-build/sysroot}
	MODULE_DIR=${MODULE_DIR:-$BUILD}
	CC=cc
fi
NIC=${NIC:-rtl8139}
[ -f "$MODULE_DIR/modules/x86_64/rtl8139.ko" ] || NIC=virtio
WORK=$BUILD/moduletest
ROOT=$WORK/root
IMAGE=$BUILD/moduletest.img
LOG=$BUILD/moduletest.log
WAIT=${WAIT:-300}

[ -f "$KERNEL" ] || { echo "moduletest: $KERNEL is missing" >&2; exit 1; }
[ -d "$SYSROOT/usr/bin" ] || { echo "moduletest: $SYSROOT has no userland" >&2; exit 1; }
[ "$ARCH" != x86_64 ] || [ -x "$LIMINE_DIR/limine" ] || { echo "moduletest: limine is missing" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$ROOT/sbin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/tmp" "$ROOT/etc" \
	"$ROOT/run/udev" "$ROOT/var/run" "$ROOT/var/db/dhcpcd" "$ROOT/var/lib/dhcpcd" \
	"$ROOT/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png \
	"$ROOT/usr/share/weston/wallpapers/tunix.png"
ln -sf usr/lib "$ROOT/lib"
ln -sf usr/lib "$ROOT/lib64"
ln -sf lib "$ROOT/usr/lib64"
ln -sf usr/bin "$ROOT/bin"

$CC -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	-Isupport/tests support/tests/moduletest.c -o "$ROOT/sbin/init" || exit 1

python3 - "$SYSROOT" "$ROOT" <<'PY' || exit 1
import os, re, shutil, subprocess, sys

sysroot, root = sys.argv[1], sys.argv[2]
programs = ['bash', 'kmod', 'lsmod', 'insmod', 'rmmod', 'modprobe', 'modinfo', 'depmod',
            'lspci', 'cat', 'ls', 'grep', 'sort', 'head', 'tail', 'wc', 'awk',
            'uname', 'sleep', 'mkdir', 'udevadm', 'udevd', 'sed', 'tr', 'dmesg',
            'basename', 'readlink', 'dirname', 'env', 'mount', 'ip', 'dhcpcd']
copied = set()

def needed(path):
    out = subprocess.run(['readelf', '-dl', path], capture_output=True, text=True).stdout
    names = re.findall(r'\(NEEDED\)[^\[]*\[(.+?)\]', out)
    interpreter = re.search(r'program interpreter: (.+?)\]', out)
    if interpreter:
        names.append(os.path.basename(interpreter.group(1)))
    return names

def copy(relative):
    if relative in copied:
        return
    source = os.path.join(sysroot, relative)
    if not os.path.lexists(source):
        return
    copied.add(relative)
    destination = os.path.join(root, relative)
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    if os.path.islink(source):
        target = os.readlink(source)
        if not os.path.lexists(destination):
            os.symlink(target, destination)
        copy(os.path.normpath(os.path.join(os.path.dirname(relative), target)))
        return
    shutil.copy2(source, destination)
    for name in needed(source):
        copy(os.path.join('usr/lib', name))

for program in programs:
    for directory in ('usr/bin', 'usr/sbin'):
        if os.path.lexists(os.path.join(sysroot, directory, program)):
            copy(os.path.join(directory, program))
            break
    else:
        print('moduletest: %s is not in the sysroot' % program, file=sys.stderr)
        sys.exit(1)

copy('usr/share/hwdata/pci.ids')
copy('etc/ld.so.cache')
copy('etc/dhcpcd.conf')
for helper in os.listdir(os.path.join(sysroot, 'usr/lib/dhcpcd/dev')):
    copy(os.path.join('usr/lib/dhcpcd/dev', helper))
for rules in ('80-drivers.rules',):
    copy(os.path.join('usr/lib/udev/rules.d', rules))
PY

echo "$NIC" > "$ROOT/nic"
cat > "$ROOT/moduletest.sh" <<'GUEST'
export PATH=/usr/bin:/usr/sbin
release=$(uname -r)
kernel=/usr/lib/modules/$release/kernel

report() { echo "MODULETEST $1 $2"; }
check() { if [ "$2" = "$3" ]; then report "$1" PASS; else report "$1" "FAIL want=$3 got=$2"; fi; }

echo "MODULETEST bash on $(uname -s) $release $(uname -m)"

check modules-empty "$(lsmod | tail -n +2 | wc -l)" 0

insmod $kernel/tunix_probe.ko number=7 flag=1 text=hello
check insmod "$?" 0
check lsmod-name "$(lsmod | awk 'NR==2 {print $1}')" tunix_probe
check param-number "$(cat /sys/module/tunix_probe/parameters/number)" 7
check param-flag "$(cat /sys/module/tunix_probe/parameters/flag)" 1
check param-text "$(cat /sys/module/tunix_probe/parameters/text)" hello
check initstate "$(cat /sys/module/tunix_probe/initstate)" live
check refcnt-idle "$(cat /sys/module/tunix_probe/refcnt)" 0

insmod $kernel/tunix_probe.ko
check insmod-twice "$?" 1

modprobe tunix_probe_user
check modprobe-user "$?" 0
check refcnt-held "$(cat /sys/module/tunix_probe/refcnt)" 1
check holders "$(ls /sys/module/tunix_probe/holders)" tunix_probe_user
check lsmod-used "$(lsmod | awk '$1 == "tunix_probe" {print $3, $4}')" "1 tunix_probe_user"

rmmod tunix_probe
check rmmod-busy "$?" 1

rmmod tunix_probe_user
check rmmod-user "$?" 0
rmmod tunix_probe
check rmmod-probe "$?" 0
check modules-gone "$(lsmod | tail -n +2 | wc -l)" 0
check sysfs-gone "$(test -d /sys/module/tunix_probe; echo $?)" 1

modprobe tunix_probe_user
check modprobe-chain "$(lsmod | tail -n +2 | wc -l)" 2
rmmod tunix_probe_user tunix_probe

check modinfo-license "$(modinfo -F license $kernel/tunix_probe.ko)" MIT
check modinfo-vermagic "$(modinfo -F vermagic $kernel/tunix_probe.ko)" "$release $(uname -m)"

head -c 512 $kernel/tunix_probe.ko > /tmp/truncated.ko
insmod /tmp/truncated.ko 2>/dev/null
check insmod-truncated "$?" 1
echo "not an object file at all" > /tmp/garbage.ko
insmod /tmp/garbage.ko 2>/dev/null
check insmod-garbage "$?" 1
sed "s/vermagic=$release/vermagic=9.9.9/" $kernel/tunix_probe.ko > /tmp/stale.ko
insmod /tmp/stale.ko 2>/dev/null
check insmod-vermagic "$?" 1
rmmod tunix_probe 2>/dev/null
check rmmod-missing "$?" 1
check survived-bad-modules "$(lsmod | tail -n +2 | wc -l)" 0

mkdir -p /etc/modprobe.d
echo "options tunix_probe number=9 text=configured" > /etc/modprobe.d/tunix.conf
modprobe tunix_probe
check modprobe-options "$(cat /sys/module/tunix_probe/parameters/number)" 9
check modprobe-options-text "$(cat /sys/module/tunix_probe/parameters/text)" configured
rmmod tunix_probe

echo "MODULETEST sysfs-pci: $(ls /sys/bus/pci/devices | tr '\n' ' ')"
lspci > /tmp/lspci.txt 2>&1
check lspci "$?" 0
echo "MODULETEST lspci:"
cat /tmp/lspci.txt
check lspci-lines "$(grep -c . /tmp/lspci.txt)" "$(ls /sys/bus/pci/devices | wc -l)"

audio=$(grep -l '^0x0403' /sys/bus/pci/devices/*/class 2>/dev/null | head -1)
audio=${audio%/class}
check audio-present "$(test -n "$audio"; echo $?)" 0
echo "MODULETEST modalias: $(cat $audio/modalias)"

modprobe snd_hda
check modprobe-snd "$?" 0
check snd-loaded "$(lsmod | awk '$1 == "snd_hda" {print $1}')" snd_hda
check snd-nodes "$(ls /dev/snd | tr '\n' ' ')" "controlC0 pcmC0D0p "
check snd-bound "$(basename $(readlink $audio/driver))" snd_hda
lspci -k > /tmp/lspcik.txt 2>/dev/null
check snd-lspci "$(grep -c 'Kernel driver in use: snd_hda' /tmp/lspcik.txt)" 1
check snd-lspci-module "$(grep -c 'Kernel modules: snd_hda' /tmp/lspcik.txt)" 1

exec 3<>/dev/snd/pcmC0D0p
check snd-refcnt-open "$(cat /sys/module/snd_hda/refcnt)" 1
rmmod snd_hda
check rmmod-open "$?" 1
exec 3<&-
check snd-refcnt-closed "$(cat /sys/module/snd_hda/refcnt)" 0

rmmod snd_hda
check rmmod-snd "$?" 0
check snd-nodes-gone "$(test -e /dev/snd/pcmC0D0p; echo $?)" 1
check snd-unbound "$(test -e $audio/driver; echo $?)" 1

udevd --daemon
udevadm trigger --action=add --type=devices
udevadm settle --timeout=30
check udev-autoload "$(lsmod | awk '$1 == "snd_hda" {print $1}')" snd_hda
check udev-nodes "$(ls /dev/snd | tr '\n' ' ')" "controlC0 pcmC0D0p "

if [ "$(cat /nic)" = rtl8139 ]; then
	check net-autoload "$(lsmod | awk '$1 == "rtl8139" {print $1}')" rtl8139
fi
check net-interface "$(grep -c eth0 /proc/net/dev)" 1
dhcpcd -1 -t 30 eth0 > /tmp/dhcpcd.log 2>&1
check net-dhcp "$?" 0
echo "MODULETEST route: $(cat /proc/net/route | tr '\t' ' ' | tr '\n' '|')"
check net-gateway "$(awk 'NR > 1 && $2 == "00000000" && $3 != "00000000" {print "yes"}' /proc/net/route | head -1)" yes

echo "MODULETEST DONE"
sleep 3600
GUEST

cat > "$WORK/limine.conf" <<CONF
timeout: 0
serial: yes

/Tunix
    protocol: limine
    path: boot():/boot/kernel.elf
    cmdline: root=LABEL=tunix-root ${EXTRA_CMDLINE:-}
CONF

ARCH=$ARCH TABLE=gpt ROOT_SLACK_MIB=16 RELEASE="$RELEASE" \
	MODULES="$MODULE_DIR/test-modules $MODULE_DIR/modules" \
	support/image.sh "$IMAGE" "$KERNEL" "$LIMINE_DIR" "$WORK/limine.conf" "$ROOT" >/dev/null || exit 1

AUDIO="-audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0"
if [ "$NIC" = virtio ]; then
	NET="-netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,netdev=net0"
else
	NET="-netdev user,id=net0 -device rtl8139,netdev=net0"
fi

rm -f "$LOG"
if [ "$ARCH" = aarch64 ]; then
	timeout "$WAIT" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 \
		-smp "${SMP:-2}" -m 2G -kernel "$KERNEL" -append "root=LABEL=tunix-root ${EXTRA_CMDLINE:-}" \
		-drive "format=raw,file=$IMAGE,if=none,id=disk0" -device nvme,drive=disk0,serial=tunix \
		$AUDIO $NET ${QEMU_EXTRA:-} -display none -no-reboot -serial "file:$LOG" >"$WORK/qemu.err" 2>&1 &
else
	ACCEL=tcg
	[ -w /dev/kvm ] && ACCEL=kvm
	timeout "$WAIT" qemu-system-x86_64 -machine "q35,accel=$ACCEL" -cpu "$([ $ACCEL = kvm ] && echo host || echo max)" \
		-smp "${SMP:-2}" -m 2G -drive "format=raw,file=$IMAGE,if=none,id=disk0" \
		-device ide-hd,drive=disk0,bus=ide.0 $AUDIO $NET ${QEMU_EXTRA:-} -display none -no-reboot \
		-serial "file:$LOG" >"$WORK/qemu.err" 2>&1 &
fi
QEMU=$!

for _ in $(seq "$WAIT"); do
	grep -aq "MODULETEST DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
sleep 1
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true

grep -aE "^(MODULETEST|TUNIXPROBE|MODULE:)" "$LOG"
python3 - "$LOG" <<'PY'
import re, sys
text = open(sys.argv[1], errors='replace').read()
failures = [line for line in text.splitlines() if re.match(r'MODULETEST \S+ FAIL', line)]
if 'MODULETEST DONE' not in text:
    failures.append('the guest did not finish')
for wanted in ('TUNIXPROBE loaded number=7 flag=1 text=hello',
               'HDA: codec',
               'TUNIXPROBE unloaded number=7',
               'TUNIXPROBEUSER loaded'):
    if wanted not in text:
        failures.append('missing kernel line: ' + wanted)
if 'PANIC' in text or 'KERNEL EXCEPTION' in text:
    failures.append('panic')
print('MODULETEST ' + ('PASS' if not failures else 'FAIL'))
for failure in failures:
    print('  ' + failure)
sys.exit(1 if failures else 0)
PY
