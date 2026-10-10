#!/bin/sh
set -eu

arch=${ARCH:-x86_64}
limine=${LIMINE:-kernel/build/limine}
cpus=${CPUS:-2}
wait=${TIMEOUT:-120}
tests=${*:-process memory files pipes sockets time}

if [ "$arch" = aarch64 ]; then
	kernel=${KERNEL:-kernel/build/kernel-aarch64-core.img}
	compiler=aarch64-linux-gnu-gcc
else
	kernel=${KERNEL:-kernel/build/kernel.elf}
	compiler=cc
fi
test -f "$kernel" || { echo "run.sh: no kernel at $kernel" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

boot() {
	if [ "$arch" = aarch64 ]; then
		timeout "$wait" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 \
			-smp "$cpus" -m 1G -kernel "$kernel" -append root=LABEL=tunix-root \
			-drive "format=raw,file=$1,if=none,id=disk0" -device nvme,drive=disk0,serial=tunix \
			-display none -no-reboot -serial "file:$2" >/dev/null 2>&1 || true
	else
		timeout "$wait" qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max \
			-smp "$cpus" -m 1G -drive "format=raw,file=$1,if=none,id=disk0" \
			-device ide-hd,drive=disk0,bus=ide.0 -display none -no-reboot \
			-serial "file:$2" >/dev/null 2>&1 || true
	fi
}

failed=
for name in $tests; do
	source=tools/tests/$name.c
	test -f "$source" || { echo "run.sh: no test called $name" >&2; exit 1; }
	dir=$work/$name
	mkdir -p "$dir/root/sbin" "$dir/root/dev" "$dir/root/proc" "$dir/root/tmp"
	$compiler -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
		-ffreestanding -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-builtin \
		-fno-asynchronous-unwind-tables "$source" -o "$dir/root/sbin/init"
	printf '%s\n' 'timeout: 0' 'serial: yes' '/Tunix' '    protocol: limine' \
		'    path: boot():/boot/kernel.elf' '    cmdline: root=LABEL=tunix-root' >"$dir/limine.conf"
	ARCH=$arch TABLE=gpt ROOT_SLACK_MIB=32 tools/image.sh "$dir/tunix.img" "$kernel" \
		"$limine" "$dir/limine.conf" "$dir/root" >"$dir/image.log" 2>&1 ||
		{ cat "$dir/image.log" >&2; exit 1; }
	boot "$dir/tunix.img" "$dir/serial.log"
	if grep -a "^$name: " "$dir/serial.log" | tee "$dir/result" | grep -q "^$name: PASS"; then
		grep -a "^$name: PASS" "$dir/result"
	else
		grep -a "^$name: FAIL" "$dir/result" || { echo "$name: no result, last serial lines:"; tail -20 "$dir/serial.log"; }
		failed="$failed $name"
	fi
done

if [ -n "$failed" ]; then
	echo "failed:$failed"
	exit 1
fi
echo "all passed"
