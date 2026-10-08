#!/bin/sh
set -u

kernel="${1:-build/kernel.elf}"
limine="${2:-build/limine}"
work="$(mktemp -d /tmp/tunix-acpitest.XXXXXX)"
trap 'rm -rf "$work"' EXIT

test -f "$kernel" || exit 1
mkdir -p "$work/root/sbin" "$work/root/proc" "$work/root/dev" "$work/root/tmp"
mkdir -p "$work/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png "$work/root/usr/share/weston/wallpapers/"
cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles -fno-stack-protector \
    -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables -Isupport/tests \
    support/tests/acpi-kerneltest.c -o "$work/root/sbin/init" || exit 1

python3 - "$work/ec.aml" <<'PY'
import struct, sys
body = b'\x08_HID\x0c\x41\xd0\x0c\x09'
header = struct.pack('<4sIBB6s8sI4sI', b'SSDT', 36 + len(body), 2, 0, b'TUNIX ', b'ECTEST  ',
                     1, b'TNX ', 1)
table = bytearray(header + body)
table[9] = (-sum(table)) & 0xFF
open(sys.argv[1], 'wb').write(table)
PY

failures=0

boot() {
    name="$1"; cmdline="$2"; extra="$3"; expect="$4"; button="$5"
    {
        echo 'timeout: 0'
        echo 'serial: yes'
        echo '/Tunix'
        echo '    protocol: limine'
        echo '    path: boot():/boot/kernel.elf'
        echo "    cmdline: root=LABEL=tunix-root hwreport $cmdline"
    } > "$work/limine.conf"
    TABLE=gpt ROOT_SLACK_MIB=16 support/image.sh "$work/tunix.img" "$kernel" "$limine" \
        "$work/limine.conf" "$work/root" >/dev/null || return 1
    rm -f "$work/serial.log" "$work/qmp.sock"
    accel=tcg; cpu=max
    test -w /dev/kvm && { accel=kvm; cpu=host; }
    timeout 120 qemu-system-x86_64 -machine "q35,accel=$accel" -cpu "$cpu" -smp 2 -m 1G \
        -drive "format=raw,file=$work/tunix.img,if=none,id=disk0" \
        -device ide-hd,drive=disk0,bus=ide.0 -display none -no-reboot \
        -qmp "unix:$work/qmp.sock,server,nowait" $extra \
        -serial "file:$work/serial.log" >/dev/null 2>&1 &
    qemu=$!
    for unused in $(seq 90); do
        grep -aq 'HWREPORT: written' "$work/serial.log" 2>/dev/null && break
        sleep 1
    done
    python3 - "$work/qmp.sock" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw'); f.readline()
for command in ('qmp_capabilities', 'system_powerdown'):
    f.write(json.dumps({'execute': command}) + '\n'); f.flush(); f.readline()
PY
    stopped=no
    for unused in $(seq 30); do
        kill -0 "$qemu" 2>/dev/null || { stopped=yes; break; }
        sleep 1
    done
    kill "$qemu" 2>/dev/null; wait "$qemu" 2>/dev/null
    key=$(grep -a '  power key' "$work/serial.log" | head -1 | sed 's/^ *power key *//')
    pressed=no
    grep -aq 'POWER: power button' "$work/serial.log" && pressed=yes
    ok=yes
    case "$key" in "$expect"*) ;; *) ok=no ;; esac
    [ "$pressed" = "$button" ] && [ "$stopped" = "$button" ] || ok=no
    if [ "$ok" = yes ]; then
        echo "ACPITEST $name PASS"
    else
        echo "ACPITEST $name FAIL key='$key' pressed=$pressed stopped=$stopped"
        grep -aE 'ACPI|POWER|THERMAL' "$work/serial.log" | head -10
        failures=$((failures + 1))
    fi
    grep -aE '^  (sci|gpe0|events) ' "$work/serial.log" | sed "s/^/ACPITEST $name /"
}

boot plain-machine "" "" "on: no embedded controller" yes
boot laptop-firmware "" "-acpitable file=$work/ec.aml" "off: the firmware has an embedded controller" no
boot laptop-forced-on "acpi_button=on" "-acpitable file=$work/ec.aml" "on (acpi_button=on)" yes
boot plain-forced-off "acpi_button=off" "" "off (acpi_button=off)" no

[ "$failures" -eq 0 ] && echo "ACPITEST PASS" || echo "ACPITEST FAIL"
[ "$failures" -eq 0 ]
