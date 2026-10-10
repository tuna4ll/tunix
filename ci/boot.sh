#!/bin/sh
set -eu

arch=${ARCH:-x86_64}
limine=${LIMINE:-kernel/build/limine}
wait=${TIMEOUT:-60}

if [ "$arch" = aarch64 ]; then
	kernel=${KERNEL:-kernel/build/kernel-aarch64-core.img}
	compiler=${AARCH64_CC:-aarch64-linux-gnu-gcc}
else
	kernel=${KERNEL:-kernel/build/kernel.elf}
	compiler=${CC:-cc}
fi
test -f "$kernel" || { echo "boot.sh: no kernel at $kernel" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-ci-boot.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

mkdir -p "$work/root/sbin" "$work/root/dev" "$work/root/proc" "$work/root/tmp"
$compiler -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-ffreestanding -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-builtin \
	-fno-asynchronous-unwind-tables utils/ci-boot/init.c -o "$work/root/sbin/init"
printf '%s\n' 'timeout: 0' 'serial: yes' '/Tunix' '    protocol: limine' \
	'    path: boot():/boot/kernel.elf' '    cmdline: root=LABEL=tunix-root' >"$work/limine.conf"
ARCH=$arch TABLE=gpt ROOT_SLACK_MIB=16 tools/image.sh "$work/tunix.img" "$kernel" \
	"$limine" "$work/limine.conf" "$work/root" >"$work/image.log" 2>&1 ||
	{ cat "$work/image.log" >&2; exit 1; }

if [ "$arch" = aarch64 ]; then
	timeout "$wait" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 \
		-smp 2 -m 1G -kernel "$kernel" -append root=LABEL=tunix-root \
		-drive "format=raw,file=$work/tunix.img,if=none,id=disk0" -device nvme,drive=disk0,serial=tunix \
		-display none -no-reboot -serial "file:$work/serial.log" >/dev/null 2>&1 || true
else
	timeout "$wait" qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max \
		-smp 2 -m 1G -drive "format=raw,file=$work/tunix.img,if=none,id=disk0" \
		-device ide-hd,drive=disk0,bus=ide.0 -display none -no-reboot \
		-serial "file:$work/serial.log" >/dev/null 2>&1 || true
fi

if grep -aq '^ci-boot: PASS' "$work/serial.log"; then
	echo "ci-boot ($arch): PASS"
else
	echo "ci-boot ($arch): FAIL, last serial lines:"
	tail -30 "$work/serial.log"
	exit 1
fi
